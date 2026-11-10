#include "media/engine/PlaybackCoordinator.hpp"
#include "media/video/TensorExport.hpp"
#include "media/ai/VoiceTemplates.hpp"
#include "media/platform/windows/Win32AacDecoder.hpp"
#include "media/platform/windows/SessionFiles.hpp"
#include "media/platform/windows/SapiSpeech.hpp"
#include "media/platform/OutputFile.hpp"
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace media::engine {
using namespace platform;
using namespace platform::windows;
namespace {
static_assert(ai::LocalIntentDispatcher::kMaxQueryBytes == subtitle::SearchIndex::kMaxQueryBytes);
namespace {
// Waits of Preparing (a read, the worker's setup): the processor is given up without Sleep(1), which lasts up to a whole
// timer tick (15.6 ms) and was most of a warm file's preparation (docs/session.md, "Warm switch").
void PrepareWait() noexcept { if (!SwitchToThread()) YieldProcessor(); }
}
// IOCP read blocks bound the largest admissible access unit (a 4K HEVC IRAP picture).
constexpr std::uint32_t kBlockBytes = 8 * 1024 * 1024, kBlocks = 16;
// AAC access units decoded ahead of a seek target to rebuild the decoder's overlap state (one suffices for
// AAC-LC; the second is margin). Their PCM, and the target unit's PCM before the target, is dropped.
constexpr std::uint32_t kAudioPreroll = 2;
// Remote sources: the file source's budget and largest admissible sample (a 4K HEVC IRAP picture), as 16 chunks of
// 8 MiB fetched progressively with read-ahead.
constexpr std::uint32_t kChunkBytes = 8 * 1024 * 1024, kChunks = 16;
// Remote sources: pictures decoded beyond the first one to show before the clock starts or restarts (or a second at
// most). A link not much faster than the programme needs this lead for the next random-access picture, which is
// large and precedes the pictures shown before it; a local file does not wait. The first picture is on screen at
// once either way; the lead delays only the moment playback starts moving.
constexpr unsigned kRemoteLead = 8;
// Pre-roll: the longest the clock waits for that lead, or for the first picture to reach the swap chain, once a
// picture to show has been decoded.
constexpr std::int64_t kPrerollLimit = 10000000;
// --test-tracks: what the committed PCM holds (channel 0). The multi-track fixture's audio tracks are each a tone of
// its own frequency, silenced for 10 ms from every 250 ms of media time. A mark is found where the tone comes back
// after a quiet run: that onset is sharp whatever the frequency, and it must keep the same place in the 250 ms grid on
// every track and across every splice (the codec's constant delay drops out: the first mark gives the grid's phase).
class ToneProbe {
public:
    using Result = PlaybackCoordinator::TrackProbe;
    void Configure(unsigned rate, unsigned channels, Result* out) noexcept { rate_ = rate; channels_ = channels; out_ = out; }
    void Origin(std::int64_t origin) noexcept { origin_ = origin; quiet_ = 0; window_ = crossings_ = 0; loud_window_ = true; previous_ = 0; }
    void Splice(std::uint64_t frame) noexcept {
        if (!out_ || out_->splice_count >= PlaybackCoordinator::kMaxSwitches) return;
        auto& s = out_->splices[out_->splice_count++]; s.frame = frame; s.dip = 1;
        float sum = 0; unsigned n = 0; // The 10 ms windows before it that held the tone throughout.
        for (unsigned i = 0; i < 5 && i < history_count_; ++i) {
            const auto& h = history_[(history_at_ + kHistory - 1 - i) % kHistory];
            if (h.loud) { sum += h.hz; ++n; }
        }
        s.before_hz = n ? sum / static_cast<float>(n) : 0;
        splice_ = frame; after_windows_ = 0; after_sum_ = 0; after_n_ = 0; measuring_ = true;
    }
    void Feed(const float* pcm, unsigned frames, std::uint64_t first) noexcept {
        if (!out_ || !rate_) return;
        const std::uint64_t around = rate_ / 50, window = rate_ / 100; // 20 ms around a splice; 10 ms windows.
        for (unsigned f = 0; f < frames; ++f) {
            const float x = pcm[static_cast<std::size_t>(f) * channels_], a = x < 0 ? -x : x;
            const std::uint64_t frame = first + f;
            level_ = (std::max)(a, level_ * 0.99995f);
            const bool splicing = splice_ != UINT64_MAX && (frame >= splice_ ? frame - splice_ : splice_ - frame) < around;
            if (splicing) { dip_peak_ = (std::max)(dip_peak_, a); if (++dip_frames_ == rate_ / 1000) { auto& s = out_->splices[out_->splice_count - 1]; s.dip = (std::min)(s.dip, dip_peak_ / (level_ + 1e-9f)); dip_peak_ = 0; dip_frames_ = 0; } }
            if (a < 0.05f * level_) ++quiet_;
            else if (a > 0.25f * level_) {
                if (quiet_ >= rate_ / 1000) {
                    const bool mark = quiet_ >= rate_ * 6 / 1000 && quiet_ <= rate_ * 14 / 1000;
                    if (splicing) {} // A splice's fade out and in.
                    else if (!mark) ++out_->gaps;
                    else {
                        // Media time of the onset, as the clock gives it, in the 250 ms grid.
                        const auto t = origin_ + static_cast<std::int64_t>(frame * 10000000 / rate_);
                        const auto phase = ((t % 2500000) + 2500000) % 2500000;
                        if (!out_->notches++) phase_ = phase;
                        auto error = phase - phase_; if (error > 1250000) error -= 2500000; if (error < -1250000) error += 2500000;
                        out_->worst_error = (std::max)(out_->worst_error, error < 0 ? -error : error);
                    }
                }
                quiet_ = 0;
            }
            if (quiet_ >= rate_ / 1000) loud_window_ = false; // A millisecond of silence (a sine's zero crossings are shorter).
            if ((x >= 0) != (previous_ >= 0)) ++crossings_;
            previous_ = x;
            if (++window_ == window) {
                const float hz = static_cast<float>(crossings_) * 0.5f * 100.0f;
                history_[history_at_] = {hz, loud_window_}; history_at_ = (history_at_ + 1) % kHistory; if (history_count_ < kHistory) ++history_count_;
                if (measuring_ && ++after_windows_ >= 2) { // Windows 2 to 6 after the splice: past its fade in.
                    if (loud_window_) { after_sum_ += hz; ++after_n_; }
                    if (after_windows_ == 6) { out_->splices[out_->splice_count - 1].after_hz = after_n_ ? after_sum_ / static_cast<float>(after_n_) : 0; measuring_ = false; }
                }
                window_ = 0; crossings_ = 0; loud_window_ = true;
            }
        }
    }
private:
    static constexpr unsigned kHistory = 16;
    struct Window { float hz; bool loud; };
    Result* out_ = nullptr;
    unsigned rate_ = 0, channels_ = 1, window_ = 0, crossings_ = 0, history_at_ = 0, history_count_ = 0, after_windows_ = 0, after_n_ = 0, dip_frames_ = 0;
    std::uint64_t quiet_ = 0, splice_ = UINT64_MAX;
    std::int64_t origin_ = 0, phase_ = 0;
    float level_ = 0, previous_ = 0, after_sum_ = 0, dip_peak_ = 0;
    bool loud_window_ = true, measuring_ = false;
    Window history_[kHistory]{};
};
}
// --test-speed: what the committed PCM holds at each speed (channel 0). The tone's frequency from the zero crossings of
// 10 ms windows that held it throughout; the 250 ms marks as dips below a quarter of the tone's level lasting 2 to 16 ms of
// media (a stretch below 1x blends a mark's silence with the tone around it: no clean silence is left, a dip is), each dip's
// centre placed in media time as the clock's segments place it and compared with the grid of the first.
// A seek's new timeline (Restart) ends the run being counted, and a run that begins at the new timeline's first frames (its
// fade-in, or the target inside a mark) is a part of a mark, not a mark: the two used to add up across the seam, and a mark cut
// short by a frame step then read as one dip at the new timeline's start, anywhere on the grid (92 and 119 ms off at 1.00x,
// about one run in fifty; docs/audio-dsp.md, "Playback speed").
class SpeedProbe {
public:
    using Record = PlaybackCoordinator::SpeedRecord;
    void Configure(unsigned rate, unsigned channels, Record* out, std::atomic<bool>* in_mark) noexcept { rate_ = rate; channels_ = channels; out_ = out; in_mark_ = in_mark; }
    void Restart() noexcept { quiet_ = 0; seam_ = rate_ / 200; clipped_ = false; }
    void Feed(const float* pcm, unsigned frames, std::int64_t media, double speed) noexcept {
        if (!out_ || !rate_) return;
        auto* a = Bucket(static_cast<float>(speed));
        const auto window = rate_ / 100;
        for (unsigned f = 0; f < frames; ++f) {
            const float x = pcm[static_cast<std::size_t>(f) * channels_], m = x < 0 ? -x : x;
            level_ = (std::max)(m, level_ * 0.99995f);
            if (m < 0.25f * level_) { if (!quiet_) clipped_ = seam_ != 0; ++quiet_; }
            else {
                if (quiet_ >= rate_ / 1000) {
                    const double quiet_media = static_cast<double>(quiet_) * speed * 1000.0 / rate_; // In media milliseconds.
                    if (a && !clipped_ && quiet_media >= 2 && quiet_media <= 16) {
                        const auto t = media + static_cast<std::int64_t>((static_cast<double>(f) - static_cast<double>(quiet_) / 2) / rate_ * speed * 10000000.0);
                        const auto phase = ((t % 2500000) + 2500000) % 2500000;
                        if (!grid_) { grid_ = true; phase_ = phase; }
                        auto error = phase - phase_; if (error > 1250000) error -= 2500000; if (error < -1250000) error += 2500000;
                        ++a->marks; a->worst = (std::max)(a->worst, error < 0 ? -error : error);
                    } else if (a) ++a->gaps;
                }
                quiet_ = 0;
            }
            if (seam_) --seam_;
            if (quiet_ >= rate_ / 1000) loud_ = false;
            if ((x >= 0) != (previous_ >= 0)) ++crossings_;
            previous_ = x;
            if (++window_ == window) {
                if (loud_ && a) { a->hz_sum += crossings_ * 0.5 * 100.0; ++a->windows; }
                window_ = 0; crossings_ = 0; loud_ = true;
            }
        }
        // For the script: the PCM committed so far ends inside a mark (its first step cuts one short on purpose).
        if (in_mark_) in_mark_->store(quiet_ >= rate_ / 1000 && quiet_ <= rate_ / 100, std::memory_order_relaxed);
    }
private:
    Record::Audio* Bucket(float speed) noexcept {
        for (unsigned i = 0; i < out_->audio_count; ++i) if (out_->audio[i].speed == speed) return out_->audio + i;
        if (out_->audio_count == std::size(out_->audio)) return nullptr;
        auto& a = out_->audio[out_->audio_count++]; a = {}; a.speed = speed; return &a;
    }
    Record* out_ = nullptr;
    std::atomic<bool>* in_mark_ = nullptr;
    unsigned rate_ = 0, channels_ = 1, window_ = 0, crossings_ = 0, seam_ = 0; // seam_: frames left of a new timeline's start.
    std::uint64_t quiet_ = 0;
    std::int64_t phase_ = 0;
    float level_ = 0, previous_ = 0;
    bool loud_ = true, grid_ = false, clipped_ = false;
};
PlaybackCoordinator::~PlaybackCoordinator() noexcept { Close(); }
std::int64_t PlaybackCoordinator::Now() const noexcept {
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value);
    return value.QuadPart / frequency_ * 10000000 + value.QuadPart % frequency_ * 10000000 / frequency_;
}
bool PlaybackCoordinator::Configuration(const demux::TrackInfo& track, bool video, std::byte* asc, unsigned* asc_bytes) noexcept {
    if (!asc) { asc = asc_; asc_bytes = &asc_bytes_; }
    for (unsigned i = 0; i < track.parameter_count; ++i) {
        const auto range = track.parameters[i].range;
        if (!range.bytes || range.bytes > 65536) return false;
        BlockToken token{}; if (source_->Request(range.offset, static_cast<std::uint32_t>(range.bytes), token) != IoStatus::Pending) return false;
        BlockView view{}; auto state = IoStatus::Pending;
        const auto deadline = GetTickCount64() + 10000;
        while (state == IoStatus::Pending && GetTickCount64() < deadline) { state = source_->Poll(token, view); if (state == IoStatus::Pending) PrepareWait(); }
        if (state != IoStatus::Ready) { (void)source_->Release(token); return false; }
        const std::span<const std::byte> nal{view.data + range.offset - view.file_offset, static_cast<std::size_t>(range.bytes)};
        bool good = false;
        if (!video) good = range.bytes >= 2 && range.bytes <= sizeof(asc_); // AudioSpecificConfig, with a PCE if present.
        else good = (hevc_ ? hevc_syntax_.ParameterSet(nal) : syntax_.ParameterSet(nal)) == codec::SyntaxStatus::Ready;
        if (!video && good) { std::memcpy(asc, nal.data(), static_cast<std::size_t>(range.bytes)); *asc_bytes = static_cast<unsigned>(range.bytes); }
        (void)source_->Release(token); if (!good) return false;
    }
    return track.parameter_count != 0;
}
std::string_view PlaybackCoordinator::ImportText(const wchar_t* path, unsigned index) noexcept {
    // A film's text samples are interleaved with its pictures over the whole file, and the block source reads whole
    // 8 MiB blocks: reading them through it would read the file. They are read here instead, before playback, with
    // positioned reads of just their bytes on a second handle (the system's cache serves them). Each sample is a
    // 16-bit length and UTF-8 text (UTF-16 with a byte order mark); style boxes after the text are not used.
    const auto* info = parser_.Track(index);
    // Matroska (API 17): Text samples are the cue's UTF-8 itself (S_TEXT/UTF8, S_TEXT/WEBVTT), Ssa samples an ASS/SSA event
    // without its times ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text": the text after the eighth comma,
    // its {...} override tags dropped, \N and \n as line breaks, \h as a space).
    const bool tx3g = info && info->codec == demux::Codec::Tx3g, ssa = info && info->codec == demux::Codec::Ssa;
    if (!info || (!tx3g && !ssa && info->codec != demux::Codec::Text)) return {};
    if (!WaitSeekable()) return {}; // A Matroska track's samples are counted by its index scan.
    if (!info->sample_count || info->sample_count > subtitle::SubtitleTrack::kMaxCues) return {};
    std::uint64_t need = 16;
    demux::SampleRecord s{};
    for (std::uint32_t i = 0; i < info->sample_count; ++i) if (parser_.SampleAt(index, i, s)) need += (std::min)(s.bytes, 8192u) * 3u / 2u + 48;
    if (need > 8u * 1024 * 1024) return {};
    auto* out = static_cast<char*>(ModuleArena(kModuleSubtitles).Allocate(static_cast<std::size_t>(need))); // Subtitles only.
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (!out || file == INVALID_HANDLE_VALUE) { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); return {}; }
    std::size_t n = 0;
    const auto put = [&](std::string_view text) { std::memcpy(out + n, text.data(), text.size()); n += text.size(); };
    const auto stamp = [&](std::int64_t t) {
        const auto ms = t / 10000; char line[24];
        const int w = std::snprintf(line, sizeof(line), "%02lld:%02lld:%02lld.%03lld", static_cast<long long>(ms / 3600000), static_cast<long long>(ms / 60000 % 60),
                                    static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000));
        put({line, static_cast<std::size_t>(w)});
    };
    put("WEBVTT\n\n");
    static std::uint8_t raw[8192 + 2]; // Preparing only, one caller.
    unsigned cues = 0;
    for (std::uint32_t i = 0; i < info->sample_count; ++i) {
        if (!parser_.SampleAt(index, i, s) || s.bytes < (tx3g ? 3u : 1u) || s.bytes > sizeof(raw) - 2) continue; // Empty samples end a cue.
        OVERLAPPED at{}; at.Offset = static_cast<DWORD>(s.offset); at.OffsetHigh = static_cast<DWORD>(s.offset >> 32);
        DWORD read = 0;
        // Text and SSA samples are read after two bytes, so that the text starts where tx3g's does (after its length).
        if (!ReadFile(file, tx3g ? raw : raw + 2, s.bytes, &read, &at) || read != s.bytes) continue;
        unsigned length = tx3g ? (std::min)(static_cast<unsigned>(raw[0] << 8 | raw[1]), s.bytes - 2u) : s.bytes;
        if (ssa) {
            // The text field, then the tags removed and the escapes resolved in place (never longer than it was).
            unsigned commas = 0, k = 0;
            while (k < length && commas < 8) if (raw[2 + k++] == ',') ++commas;
            unsigned kept = 0; bool tag = false;
            for (; k < length; ++k) {
                const auto c = raw[2 + k];
                if (tag) { tag = c != '}'; continue; }
                if (c == '{') { tag = true; continue; }
                if (c == '\\' && k + 1 < length && (raw[3 + k] == 'N' || raw[3 + k] == 'n')) { raw[2 + kept++] = '\n'; ++k; continue; }
                if (c == '\\' && k + 1 < length && raw[3 + k] == 'h') { raw[2 + kept++] = ' '; ++k; continue; }
                raw[2 + kept++] = c;
            }
            length = commas == 8 ? kept : 0;
        }
        const std::int64_t start = (std::max)(s.pts_100ns, std::int64_t{0}), end = s.pts_100ns + s.duration_100ns;
        if (!length || end <= start) continue;
        // The text, one line per line: carriage returns dropped, no empty line (it would end the cue), no edge blanks.
        char cue[8192 * 3 / 2 + 8]; unsigned t = 0;
        const auto emit = [&](std::uint32_t c) {
            if (c == '\r' || c == 0) return;
            if (c == '\n' && (t == 0 || cue[t - 1] == '\n')) return;
            t += text::Encode(c, cue + t);
        };
        if (length >= 2 && raw[2] == 0xfe && raw[3] == 0xff) { // UTF-16BE.
            for (unsigned k = 2; k + 1 < length; k += 2) {
                std::uint32_t c = static_cast<std::uint32_t>(raw[2 + k] << 8 | raw[3 + k]);
                if (c >= 0xd800 && c < 0xdc00 && k + 3 < length) {
                    const std::uint32_t low = static_cast<std::uint32_t>(raw[4 + k] << 8 | raw[5 + k]);
                    if (low >= 0xdc00 && low < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00); k += 2; } else c = 0xfffd;
                } else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
                emit(c);
            }
        } else {
            std::string_view rest{reinterpret_cast<const char*>(raw + 2), length}; std::uint32_t c = 0;
            while (!rest.empty()) { if (!text::Utf8(rest, c)) { rest.remove_prefix(1); c = '?'; } emit(c); } // Never longer than its bytes.
        }
        while (t && (cue[t - 1] == '\n' || cue[t - 1] == ' ')) --t;
        if (!t) continue;
        stamp(start); put(" --> "); stamp(end); put("\n"); put({cue, t}); put("\n\n"); ++cues;
    }
    CloseHandle(file);
    return cues ? std::string_view{out, n} : std::string_view{};
}
namespace {
// Intents whose execution is a seek (its landing is recorded for scripted commands).
bool SeekingIntent(ai::IntentKind kind) noexcept {
    using ai::IntentKind;
    return kind == IntentKind::SeekRelative || kind == IntentKind::SeekAbsolute || kind == IntentKind::FindSubtitle || kind == IntentKind::NextChapter ||
           kind == IntentKind::PreviousChapter || kind == IntentKind::NextScene || kind == IntentKind::PreviousScene || kind == IntentKind::ReplayCue;
}
}
namespace {
// Whether the platform has a software AV1 decoder (a Media Foundation transform): asked once a process (API 17).
bool Av1Decodable() noexcept {
    static std::atomic<int> known{-1};
    int value = known.load(std::memory_order_acquire);
    if (value < 0) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        value = SUCCEEDED(mf) && Win32VideoMftDecoder::Available(MFVideoFormat_AV1) ? 1 : 0;
        if (SUCCEEDED(mf)) (void)MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        known.store(value, std::memory_order_release);
    }
    return value == 1;
}
}
bool PlaybackCoordinator::WaitSeekable() noexcept {
    // Sample lookups need every sample proven first (the scan's validation pass; it publishes no packet).
    const auto deadline = GetTickCount64() + 5000;
    while (!parser_.Indexed() && GetTickCount64() < deadline) { // The complete index (a Matroska file's whole scan): sample lookups.
        const auto status = parser_.Step(*packets_);
        if (status == demux::ParseStatus::Failed) return false;
        if (status == demux::ParseStatus::NeedInput) { PumpSource(); Sleep(0); }
    }
    return parser_.Indexed();
}
void PlaybackCoordinator::ImportChapters(const wchar_t* path) noexcept {
    // Like ImportText: positioned reads of just the samples' bytes. Each sample is a 16-bit length and the chapter's
    // title (UTF-8, or UTF-16 with a byte order mark); its time is the chapter's start.
    const unsigned index = parser_index_[chapter_catalogue_];
    const auto* info = parser_.Track(index);
    if (!info || !info->sample_count || !WaitSeekable()) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    demux::SampleRecord s{};
    for (std::uint32_t i = 0; i < info->sample_count && timeline_.Chapters() < timeline::ChapterTimeline::kMaxChapters; ++i) {
        std::uint8_t raw[2 + 512]; char title[sizeof(timeline::Chapter::title)]; unsigned t = 0;
        if (!parser_.SampleAt(index, i, s) || s.bytes < 2) continue;
        const auto bytes = (std::min)(s.bytes, static_cast<std::uint32_t>(sizeof(raw)));
        OVERLAPPED at{}; at.Offset = static_cast<DWORD>(s.offset); at.OffsetHigh = static_cast<DWORD>(s.offset >> 32);
        DWORD read = 0;
        if (!ReadFile(file, raw, bytes, &read, &at) || read != bytes) continue;
        const unsigned length = (std::min)(static_cast<unsigned>(raw[0] << 8 | raw[1]), bytes - 2u);
        const auto put = [&](std::uint32_t c) {
            char encoded[4]; const unsigned n = text::Encode(c < 0x20 ? ' ' : c, encoded);
            if (t + n < sizeof(title)) { std::memcpy(title + t, encoded, n); t += n; }
        };
        if (length >= 2 && raw[2] == 0xfe && raw[3] == 0xff) {
            for (unsigned k = 2; k + 1 < length; k += 2) {
                std::uint32_t c = static_cast<std::uint32_t>(raw[2 + k] << 8 | raw[3 + k]);
                if (c >= 0xd800 && c < 0xe000) c = 0xfffd; // Titles of the BMP only; a surrogate pair is replaced.
                put(c);
            }
        } else {
            std::string_view rest{reinterpret_cast<const char*>(raw + 2), length}; std::uint32_t c = 0;
            while (!rest.empty()) { if (!text::Utf8(rest, c)) { rest.remove_prefix(1); c = '?'; } put(c); }
        }
        (void)timeline_.AddChapter((std::max)(s.pts_100ns, std::int64_t{0}), {title, t});
    }
    CloseHandle(file);
    if (timeline_.Chapters()) timeline_.SetSource(timeline::ChapterSource::QuickTime);
}
void PlaybackCoordinator::PrepareTimeline(const wchar_t* path) noexcept {
    // Chapters (docs/chapters.md): the file's own (Nero first, then a QuickTime chapter track), then scenes and, without
    // chapters, sections from the structure: sync samples (their cadence and sizes) and dialogue gaps. Preparing only:
    // the scratch arrays come from the arena.
    const auto begin = Now();
    timeline_.Clear();
    timeline_.SetDuration(video_.duration_100ns > 0 ? video_.duration_100ns : duration_);
    for (unsigned c = 0; c < parser_.ChapterCount(); ++c) {
        const auto& chapter = parser_.ChapterAt(c);
        (void)timeline_.AddChapter(chapter.start_100ns, chapter.title);
    }
    if (timeline_.Chapters()) timeline_.SetSource(timeline::ChapterSource::Nero);
    else if (chapter_catalogue_ >= 0 && !remote_) ImportChapters(path);
    // A remote source is not waited for: its scan reads the network. Its structural marks are left out (cuts seen while
    // playing still come).
    if ((!remote_ || parser_.Indexed()) && WaitSeekable()) {
        constexpr std::uint32_t kMaxKeyframes = 65536;
        const auto syncs = video_.id ? parser_.SyncCount(video_parser_) : 0u;
        const auto stride = syncs == UINT32_MAX ? 1u : (syncs + kMaxKeyframes - 1) / kMaxKeyframes;
        const auto count = syncs == UINT32_MAX ? 0u : syncs / (stride ? stride : 1u);
        auto& arena = ModuleArena(kModuleChapters);
        auto* keys = count ? static_cast<timeline::Keyframe*>(arena.Allocate(sizeof(timeline::Keyframe) * count, alignof(timeline::Keyframe))) : nullptr;
        unsigned keyframes = 0;
        demux::SampleRecord s{};
        for (std::uint32_t k = 0; keys && k < count; ++k)
            if (parser_.SampleAt(video_parser_, parser_.SyncAt(video_parser_, k * stride), s)) keys[keyframes++] = {s.pts_100ns, s.bytes};
        // Dialogue: the cues of one subtitle source (the one shown at start, else the first), in time order.
        const auto cues = subtitles_.Cues();
        auto* spans = cues.empty() ? nullptr : static_cast<timeline::Interval*>(arena.Allocate(sizeof(timeline::Interval) * cues.size(), alignof(timeline::Interval)));
        unsigned spoken = 0; std::uint8_t channel = cues.empty() ? 0 : cues.front().channel;
        const auto* shown = subtitle_choice_ != kNoTrack ? FindTrack(subtitle_choice_, demux::TrackKind::Subtitle) : nullptr;
        if (shown) channel = shown->slot;
        for (const auto& cue : cues) if (spans && cue.channel == channel) spans[spoken++] = {cue.start, cue.end};
        synthesis_ = timeline::Synthesize({keys, keyframes, spans, spoken, timeline_.Duration()}, timeline_);
    }
    timeline_prepare_ = Now() - begin;
    PackMarks();
}
void PlaybackCoordinator::PackMarks() noexcept {
    // Chapter ticks first (not the one at the start), then scene dots, evenly picked when there are more than fit, and
    // none where a chapter tick already is.
    ui::TimelineMarks marks; const auto duration = timeline_.Duration();
    if (duration > 0) {
        for (unsigned c = 0; c < timeline_.Chapters() && marks.count < ui::TimelineMarks::kMax; ++c)
            if (timeline_.ChapterAt(c).start > 0) (void)marks.Add(static_cast<double>(timeline_.ChapterAt(c).start) / static_cast<double>(duration), true);
        const unsigned room = ui::TimelineMarks::kMax - marks.count, scenes = timeline_.Scenes();
        for (unsigned k = 0; k < room && k < scenes; ++k) {
            const auto& scene = timeline_.SceneAt(scenes <= room ? k : static_cast<unsigned>(std::uint64_t{k} * scenes / room));
            bool beside = false;
            for (unsigned c = 0; c < timeline_.Chapters() && !beside; ++c) {
                const auto d = timeline_.ChapterAt(c).start - scene.at;
                beside = (d < 0 ? -d : d) < timeline::ChapterTimeline::kSceneMerge;
            }
            if (!beside) (void)marks.Add(static_cast<double>(scene.at) / static_cast<double>(duration), false);
        }
    }
    renderer_.SetTimelineMarks(marks);
}
ai::IntentStatus PlaybackCoordinator::Navigate(bool chapter, int direction) noexcept {
    NavigationResult r; r.chapter = chapter; r.direction = direction > 0 ? 1 : -1;
    r.from = Bounds().position_100ns;
    const bool found = chapter ? (direction > 0 ? timeline_.NextChapter(r.from, r.index) : timeline_.PreviousChapter(r.from, r.index))
                               : (direction > 0 ? timeline_.NextScene(r.from, r.index) : timeline_.PreviousScene(r.from, r.index));
    // The cue, assembled in place: "Chapter 3 · Finale · 02:15", "Section 2 · 10:00", "Scene 7 · 1:02:03", "No next scene".
    unsigned n = 0;
    const auto put = [&](std::string_view part) { for (const char c : part) if (n + 1 < sizeof(r.cue)) r.cue[n++] = c; };
    const auto number = [&](std::uint64_t value, unsigned digits) {
        char d[24]; unsigned k = 0;
        do { d[k++] = static_cast<char>('0' + value % 10); value /= 10; } while ((value || k < digits) && k < 20);
        while (k) { if (n + 1 < sizeof(r.cue)) r.cue[n++] = d[k - 1]; --k; }
    };
    if (!found) {
        put("No "); put(direction > 0 ? "next " : "previous ");
        put(!chapter ? "scene" : timeline_.Source() == timeline::ChapterSource::Synthesized ? "section" : "chapter");
        r.status = ai::IntentStatus::NotFound;
        Status(r.cue, ui::StatusIcon::Warning, ui::StatusTone::Warning);
    } else {
        r.target = chapter ? timeline_.ChapterAt(r.index).start : timeline_.SceneAt(r.index).at;
        put(!chapter ? "Scene " : timeline_.Source() == timeline::ChapterSource::Synthesized ? "Section " : "Chapter ");
        number(r.index + 1, 1); put(" \xc2\xb7 ");
        if (chapter && timeline_.ChapterAt(r.index).title[0]) { put(timeline_.ChapterAt(r.index).title); put(" \xc2\xb7 "); }
        const auto seconds = static_cast<std::uint64_t>(r.target / 10000000);
        if (seconds >= 3600) { number(seconds / 3600, 1); put(":"); }
        number(seconds / 60 % 60, 2); put(":"); number(seconds % 60, 2);
        if (RequestSeek(r.target)) { r.epoch = epoch_; Status(r.cue, ui::StatusIcon::None, ui::StatusTone::Info); }
        else r.status = ai::IntentStatus::Rejected;
    }
    r.cue[n] = 0;
    if (navigation_count_ < kMaxNavigations) navigations_[navigation_count_++] = r;
    return r.status;
}
PlaybackCoordinator::PlaybackCoordinator(WarmSession* warm) noexcept
    : warm_(warm), arena_(warm ? warm->arena : own_arena_), renderer_(warm ? &warm->gpu : nullptr), decoder_(warm ? warm->decoder : own_decoder_) {}
WarmSession::WarmSession() noexcept { if (FAILED(CoIncrementMTAUsage(&mta_))) mta_ = nullptr; }
void WarmSession::Close() noexcept {
    // After the last coordinator closed: the decoder first (it holds the device), then the GPU objects and the window,
    // the endpoint, the arenas.
    if (decoder.Prepared() && decoder.Close() != platform::windows::DecodeStatus::Ready) decoder.QuarantineGpuResources();
    gpu.Close();
    if (sink_ready) { sink.Close(); sink_ready = false; }
    (void)arena.Shutdown(); (void)decoder_arena.Shutdown(); (void)sink_arena.Shutdown();
    if (mta_) { (void)CoDecrementMTAUsage(mta_); mta_ = nullptr; }
}
bool PlaybackCoordinator::Prepare(const wchar_t* path, PlaybackOptions options) noexcept {
    (void)SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); // Opening a file is activity: the idle countdown restarts.
    options_ = options;
    stop_requested_.store(false, std::memory_order_relaxed); cures_.Reset();
    signature_count_.store(0, std::memory_order_relaxed); signatures_lost_.store(0, std::memory_order_relaxed);
    LARGE_INTEGER frequency{}; if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || frequency.QuadPart > INT64_MAX / 10000000) return false;
    frequency_ = frequency.QuadPart;
    prepare_times_ = {};
    const auto preparing = Now(); auto stage = preparing;
    const auto lap = [&](std::int64_t& into) { const auto now = Now(); into = now - stage; stage = now; };
    // 128 MiB of read blocks plus demux tables, codec tables and queues; eagerly committed before sealing. A warm session's
    // arena is committed already: rewound, it costs nothing.
    if (warm_) ++warm_->reuse.files;
    // The core and each enabled module's region (docs/sdk.md, "Modules"): ArenaBudget(modules) exactly. A warm session's
    // arena serves when it has that size (another module set makes it again).
    const auto budget = static_cast<std::size_t>(ArenaBudget(options.modules));
    for (auto& view : module_arenas_) (void)view.Shutdown(); // The previous file's regions.
    if (warm_ && arena_.Statistics().capacity_bytes == budget) { if (!arena_.Reset()) return false; ++warm_->reuse.arenas_kept; }
    else if ((arena_.Statistics().capacity_bytes && !arena_.Shutdown()) || !arena_.Initialize(budget)) return false;
    if (!PrepareModules()) return false;
    lap(prepare_times_.arena);
    // http:// and https:// stream through ranged GETs into the same arena budget; anything else is a file.
    remote_ = _wcsnicmp(path, L"http://", 7) == 0 || _wcsnicmp(path, L"https://", 8) == 0;
    if (remote_) {
        HttpSourceOptions network; network.chunk_count = kChunks;
        network.chunk_bytes = options.network_chunk_bytes ? options.network_chunk_bytes : kChunkBytes;
        network.overlap_bytes = network.chunk_bytes / 8; network.read_bytes = (std::min)(network.read_bytes, network.chunk_bytes / 4);
        network.headers = options.http_headers; network.cancel = options.cancel;
        source_ = &http_;
        const auto opened = http_.Open(path, arena_, network);
        if (opened == IoStatus::Cancelled) { Fail(1, E_ABORT); return false; }
        if (opened != IoStatus::Ready) {
            // An HTTP status the server answered (401, 403, 404, a redirect not followed) says more than the OS error 0.
            const auto status = http_.LastHttpStatus();
            Fail(1, status >= 300 ? static_cast<HRESULT>(status) : HRESULT_FROM_WIN32(http_.LastOsError())); return false;
        }
    } else if (file_.Open(path, arena_, {kBlockBytes, kBlocks, false}) != IoStatus::Ready) { Fail(1, HRESULT_FROM_WIN32(file_.LastOsError())); return false; }
    lap(prepare_times_.open);
    // Smart resume (docs/session.md): this file's entry in the ledger (local files: their full path and size).
    resume_ = {}; file_request_ = FileRequest::None;
    if (options.resume && !remote_ && platform::windows::FileIdentity(path, resume_key_, resume_bytes_))
        if (const auto* entry = options.resume->Find(resume_key_, resume_bytes_)) {
            resume_.known = true; resume_.entry = *entry;
            bookmarks_.Load(entry->bookmark_count, entry->bookmarks); // As they were left (docs/session.md, "Bookmarks").
        }
    {
        const wchar_t* name = path;
        for (const wchar_t* c = path; *c; ++c) if (*c == L'\\' || *c == L'/') name = c + 1;
        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, file_name_, static_cast<int>(sizeof(file_name_)), nullptr, nullptr) <= 0) file_name_[0] = 0;
    }
    packets_ = arena_.Create<core::SpscRingBuffer<core::MediaPacket, 8>>();
    packet_returns_ = arena_.Create<core::SpscRingBuffer<core::MediaPacket, 32>>();
    frames_ = arena_.Create<core::SpscRingBuffer<core::VideoFrameDescriptor, 32>>();
    surface_returns_ = arena_.Create<core::SpscRingBuffer<VideoSurfaceReturn, 64>>();
    controls_ = arena_.Create<core::SpscRingBuffer<ControlCommand, 16>>();
    if (!packets_ || !packet_returns_ || !frames_ || !surface_returns_ || !controls_ ||
        !parser_.Initialize(arena_, *source_, {16, demux::ParserLimits{}.index_bytes, 1, !remote_})) { Fail(2); return false; } // Remote: no read-ahead scan (API 17).
    const auto deadline = GetTickCount64() + 20000;
    while (!parser_.MetadataReady() && GetTickCount64() < deadline) {
        if (options.cancel && options.cancel->load(std::memory_order_acquire)) { Fail(1, E_ABORT); return false; } // API 15.
        const auto status = parser_.Step(*packets_);
        if (status == demux::ParseStatus::Failed || status == demux::ParseStatus::Done) { Fail(3, static_cast<HRESULT>(parser_.Diagnostic().error)); return false; }
        if (status == demux::ParseStatus::NeedInput) PrepareWait();
    }
    if (!parser_.MetadataReady()) { Fail(3); return false; }
    lap(prepare_times_.metadata);
    // The catalogue (docs/tracks.md): every track, whether it can be used and how. One video track plays (the first
    // decodable one, the file's default first); every AAC track of a layout the platform decodes can be the audio.
    using demux::TrackKind;
    int video_index = -1;
    for (unsigned i = 0; i < parser_.TrackCount() && track_count_ < kMaxTracks; ++i) {
        const auto& t = *parser_.Track(i);
        auto& m = tracks_[track_count_]; parser_index_[track_count_] = static_cast<std::uint8_t>(i); ++track_count_;
        m.id = t.id; m.kind = t.kind; m.codec = t.codec; m.sample_entry = t.sample_entry; ai::CanonicalLanguage(t.language, m.language);
        // Muxers put their own name in the handler ("SoundHandler", "Core Media Audio", "GPAC ISO Video Handler"): not a label.
        const std::string_view name{t.name};
        const bool generic = name.empty() || name.find("andler") != name.npos || name.find("Media") != name.npos || name.find("ISO") != name.npos;
        if (!generic) std::memcpy(m.name, t.name, sizeof(m.name));
        m.channels = t.channels; m.sample_rate = t.sample_rate; m.width = t.width; m.height = t.height;
        m.alternate_group = t.alternate_group; m.enabled = t.enabled;
        m.use = t.refusal != demux::ParseError::None ? TrackUse::Refused : TrackUse::Undecodable;
        if (m.use == TrackUse::Refused) continue;
        // AV1: the platform's software decoder (ENGINE_MANIFESTO.md 2E: this GPU has no AV1 decode; hardware decoding is used for the rest).
        const bool decodable_video = t.kind == TrackKind::Video && (t.codec == demux::Codec::H264 || t.codec == demux::Codec::Hevc ||
                                                                    (t.codec == demux::Codec::Av1 && Av1Decodable()));
        if (decodable_video && (video_index < 0 || (t.enabled && !video_.enabled))) {
            if (video_index >= 0) { tracks_[video_index].use = TrackUse::Incompatible; (void)parser_.SetEmission(parser_index_[video_index], false); }
            video_ = t; video_index = static_cast<int>(track_count_ - 1); m.use = TrackUse::Selectable;
        } else if (t.kind == TrackKind::Video) {
            m.use = decodable_video ? TrackUse::Incompatible : TrackUse::Undecodable; // A second picture track, AV1, ...
            (void)parser_.SetEmission(i, false);
        } else if (t.kind == TrackKind::Audio) {
            if ((t.codec == demux::Codec::Aac || t.codec == demux::Codec::Opus) && audio_count_ < kMaxAudioTracks) { m.slot = static_cast<std::uint8_t>(audio_count_); audio_tracks_[audio_count_++] = t; }
            (void)parser_.SetEmission(i, false); // The one to play is chosen below.
        } else if (t.kind == TrackKind::Subtitle) m.use = TrackUse::Selectable; // Until its samples prove otherwise.
    }
    // Music mode (docs/audio-dsp.md, "Music mode"): without a picture track an AAC track plays alone and the window shows
    // its sound. A picture track that cannot be decoded (AV1, ...) is refused as before: that file is a film, not music.
    // Neither: nothing to play.
    bool pictures = false;
    for (unsigned c = 0; c < track_count_; ++c) pictures = pictures || tracks_[c].kind == TrackKind::Video;
    if (!video_.id && pictures) { Fail(4); return false; }
    music_ = !video_.id;
    if (music_ && !audio_count_) { Fail(4); return false; }
    // A QuickTime chapter track (tref 'chap' of the video, or of the audio in music mode): its samples name the chapters,
    // so it is no subtitle track.
    const auto chapter_track = music_ ? audio_tracks_[0].chapter_track : video_.chapter_track;
    for (unsigned c = 0; chapter_track && c < track_count_; ++c)
        if (tracks_[c].id == chapter_track && tracks_[c].kind == TrackKind::Subtitle && tracks_[c].use != TrackUse::Refused) {
            tracks_[c].use = TrackUse::Chapters; chapter_catalogue_ = static_cast<int>(c);
        }
    if (!music_) video_parser_ = parser_index_[video_index];
    continuation_.store(options.seek_continuation, std::memory_order_relaxed);
    queue_seek_.store(options.queue_seek, std::memory_order_relaxed); scrub_preview_.store(options.scrub_preview, std::memory_order_relaxed);
    // Only the selected codec's syntax tables are allocated.
    hevc_ = video_.codec == demux::Codec::Hevc;
    av1_ = video_.codec == demux::Codec::Av1;
    if (av1_) {
        // No bitstream syntax here: the decoder is the platform's. Seek continuation, queue seeks and scrub previews keep a
        // hardware decoder's chain and are not used with it.
        continuation_.store(false, std::memory_order_relaxed); queue_seek_.store(false, std::memory_order_relaxed);
        scrub_preview_.store(false, std::memory_order_relaxed);
    } else if (!music_) {
        if (hevc_) hevc_dxva_ = arena_.Create<codec::HevcDxva>(); else dxva_ = arena_.Create<codec::H264Dxva>();
        if (hevc_ ? !hevc_dxva_ || !hevc_syntax_.Initialize(arena_) : !dxva_ || !syntax_.Initialize(arena_)) { Fail(2); return false; }
        if (!Configuration(video_, true)) { Fail(5); return false; }
    }
    // Audio: every AAC track's AudioSpecificConfig; a layout the platform decodes makes it a candidate. The starting
    // track: the language asked for, else the file's default (tkhd enabled), else the first. A track that cannot be
    // decoded never stops playback: without any, the endpoint's silence clocks the video (UnsupportedLayout).
    int chosen = -1, first_undecodable = -1;
    char wanted[4]{}; ai::CanonicalLanguage(options.audio_language, wanted);
    for (unsigned c = 0; c < track_count_; ++c) {
        auto& m = tracks_[c];
        if (m.kind != TrackKind::Audio || (m.codec != demux::Codec::Aac && m.codec != demux::Codec::Opus) || m.use == TrackUse::Refused || m.slot >= audio_count_) continue;
        const auto slot = m.slot;
        codec::AacConfig config{};
        bool decodable = false;
        if (m.codec == demux::Codec::Opus) { // The platform's Opus decoder: mono or stereo, 48 kHz (API 17).
            config.channels = audio_tracks_[slot].channels; config.sample_rate = 48000; asc_sizes_[slot] = 0;
            decodable = config.channels == 1 || config.channels == 2;
        } else {
            const bool read = Configuration(audio_tracks_[slot], false, ascs_[slot], &asc_sizes_[slot]);
            decodable = read && codec::ParseAacConfig({ascs_[slot], asc_sizes_[slot]}, config) == codec::AacConfigStatus::Ready &&
                        config.channels <= Win32AacDecoder::kPlatformChannels;
        }
        if (!decodable) { if (first_undecodable < 0) first_undecodable = static_cast<int>(c); continue; }
        m.use = TrackUse::Selectable;
        m.channels = config.channels; // The ASC decides, not the sample entry (muxers write 2 for 5.1 too).
        const auto better = [&](int current) {
            if (current < 0) return true;
            const auto& o = tracks_[current];
            if (const auto kept = resume_.known ? resume_.entry.audio_track : 0u; kept && (m.id == kept) != (o.id == kept)) return m.id == kept;
            const bool language = wanted[0] && std::strcmp(m.language, wanted) == 0, other_language = wanted[0] && std::strcmp(o.language, wanted) == 0;
            if (language != other_language) return language;
            return m.enabled && !o.enabled;
        };
        if (better(chosen)) chosen = static_cast<int>(c);
    }
    resume_.restored_audio = resume_.known && chosen >= 0 && resume_.entry.audio_track && tracks_[chosen].id == resume_.entry.audio_track;
    if (chosen >= 0) {
        const auto slot = tracks_[chosen].slot;
        audio_ = audio_tracks_[slot]; first_slot_ = slot; audio_choice_ = tracks_[chosen].id;
        std::memcpy(asc_, ascs_[slot], asc_sizes_[slot]); asc_bytes_ = asc_sizes_[slot];
        (void)parser_.SetEmission(parser_index_[chosen], true);
    } else if (first_undecodable >= 0) {
        // Reported as before: the layout of the file's audio, which plays as silence.
        const auto slot = tracks_[first_undecodable].slot;
        audio_ = audio_tracks_[slot]; std::memcpy(asc_, ascs_[slot], asc_sizes_[slot]); asc_bytes_ = asc_sizes_[slot];
    }
    if (music_ && chosen < 0) { Fail(4); return false; } // Music mode needs sound it can decode.
    unsigned left = 0, top = 0, right = 0, bottom = 0, reorder = 0, references = 16;
    auto& described = picture_; described = {};
    const auto bitstream = [&](unsigned primaries, unsigned transfer, unsigned matrix, bool full_range, bool signal, bool colour, unsigned sar_width, unsigned sar_height) {
        described.bitstream_color = {static_cast<std::uint8_t>(primaries), static_cast<std::uint8_t>(transfer), static_cast<std::uint8_t>(matrix), full_range, colour, signal};
        described.bitstream_aspect = {sar_width, sar_height};
    };
    if (music_) {
        // No picture: nothing to describe.
    } else if (av1_) {
        // av1C (ISO/IEC 14496 AV1 binding, also Matroska's CodecPrivate): marker and version, seq_profile, then the depth and
        // chroma flags. Main profile 4:2:0, 8 or 10 bits; the picture's size from the container.
        std::byte config[4]{};
        BlockToken token{}; BlockView view{}; auto state = IoStatus::Pending;
        if (video_.configuration.bytes < 4 || source_->Request(video_.configuration.offset, 4, token) != IoStatus::Pending) { Fail(5); return false; }
        const auto until = GetTickCount64() + 10000;
        while (state == IoStatus::Pending && GetTickCount64() < until) { state = source_->Poll(token, view); if (state == IoStatus::Pending) PrepareWait(); }
        if (state == IoStatus::Ready) std::memcpy(config, view.data + (video_.configuration.offset - view.file_offset), 4);
        (void)source_->Release(token);
        const unsigned profile = std::to_integer<unsigned>(config[1]) >> 5, flags = std::to_integer<unsigned>(config[2]);
        const bool high = (flags & 0x40) != 0, twelve = (flags & 0x20) != 0, mono = (flags & 0x10) != 0, x = (flags & 0x08) != 0, y = (flags & 0x04) != 0;
        if (state != IoStatus::Ready || std::to_integer<unsigned>(config[0]) != 0x81 || profile != 0 || twelve || mono || !x || !y) { Fail(5); return false; }
        width_ = (video_.width + 1u) & ~1u; height_ = (video_.height + 1u) & ~1u; ten_bit_ = high;
        left = top = 0; right = video_.width; bottom = video_.height;
        bitstream(2, 2, 2, false, false, false, 0, 0); // The sequence header's colour is not read: the container's, else inferred.
        reorder = 0; references = 8; // Pictures come out of the transform in presentation order.
    } else if (hevc_) {
        const auto* sps = hevc_syntax_.FirstSps(); if (!sps) { Fail(5); return false; }
        // PQ (16) and HLG (18) are tone-mapped on the GPU (render::ColorPipeline); stage 31 is no longer used.
        width_ = sps->width; height_ = sps->height; ten_bit_ = sps->bit_depth_luma_minus8 == 2;
        left = sps->left; top = sps->top; right = sps->right; bottom = sps->bottom;
        bitstream(sps->colour_primaries, sps->transfer, sps->matrix, sps->full_range, sps->signal_described, sps->colour_described, sps->sar_width, sps->sar_height);
        reorder = sps->max_num_reorder; references = sps->max_dec_pic_buffering_minus1 + 1u;
    } else {
        const auto* sps = syntax_.FirstSps(); if (!sps) { Fail(5); return false; }
        width_ = sps->width; height_ = sps->height;
        left = sps->left; top = sps->top; right = sps->right; bottom = sps->bottom;
        bitstream(sps->primaries, sps->transfer, sps->matrix, sps->full_range, sps->signal_described, sps->colour_described, sps->sar_width, sps->sar_height);
        // B-pictures present out of decode order: the VUI's max_num_reorder_frames, else the Annex E inference.
        reorder = (std::min)(unsigned{sps->reorder}, render::D3D12Renderer::Count - 2);
        references = (std::max)(unsigned{sps->max_refs}, 1u);
    }
    // The picture as described: the crop, the shape of its samples and its colour, each from the container where it
    // says something and from the bitstream's VUI otherwise; what neither says is inferred (docs/picture.md).
    const unsigned crop_width = right - left, crop_height = bottom - top;
    described.coded_width = width_; described.coded_height = height_;
    described.left = left; described.top = top; described.right = right; described.bottom = bottom;
    const auto code = [](std::uint16_t v) { return static_cast<std::uint8_t>(v > 255 ? video::kUnspecified : v); };
    described.container_color = {code(video_.color.primaries), code(video_.color.transfer), code(video_.color.matrix), video_.color.full_range,
                                 video_.color.described, video_.color.range_described};
    described.container_aspect = {video_.pixel_aspect_width, video_.pixel_aspect_height};
    described.color = video::ResolveColor(described.container_color, described.bitstream_color, crop_height);
    described.sample_aspect = video::ResolveSampleAspect(described.container_aspect, described.bitstream_aspect, &described.aspect_source);
    described.display = video::DisplaySize(crop_width, crop_height, described.sample_aspect);
    if (!music_ && !video::ConvertibleMatrix(described.color.matrix)) { Fail(5); return false; } // Identity (RGB), YCgCo, constant luminance, ICtCp.
    if (reorder + 2 > render::D3D12Renderer::Count) { Fail(32, static_cast<HRESULT>(reorder)); return false; }
    // Window: the picture's display shape (its crop in square pixels), at least 640 wide, within three quarters of the
    // primary display.
    // A file shown a quarter turn round (its tkhd matrix: docs/picture.md, "Rotation") opens in its shown shape.
    renderer_.SetOrientation(video_.transform);
    const bool across = !music_ && video_.transform[0] == 0;
    const unsigned width = music_ ? 960u : across ? described.display.height : described.display.width,
                   height = music_ ? 540u : across ? described.display.width : described.display.height; // Music: 16:9.
    const unsigned screen_width = static_cast<unsigned>((std::max)(GetSystemMetrics(SM_CXSCREEN), 1280)) * 3 / 4;
    const unsigned screen_height = static_cast<unsigned>((std::max)(GetSystemMetrics(SM_CYSCREEN), 720)) * 3 / 4;
    unsigned display_width = (std::clamp)(width, 640u, screen_width), display_height = height * display_width / width;
    if (display_height > screen_height) { display_width = display_width * screen_height / display_height; display_height = screen_height; }
    // Decode surfaces: DPB plus current picture plus in-flight conversions, never more than the backend admits.
    const unsigned surfaces = music_ ? 1u : av1_ ? 16u : hevc_ ? (std::min)(references + 1u + 8u, 32u) : (std::clamp)(references + 1u + 8u, 24u, 32u);
    if (options.test_paused_quality || options.test_zoom || options.test_bookmarks || options.test_rotation || options.test_auto_horizon) renderer_.EnableCapture();
    if (options.test_auto_horizon) renderer_.EnableCaptureHorizon();
    if (options.test_stabilize) { renderer_.EnableCapture(); renderer_.EnableSpotCapture(); }
    renderer_.Adopt(options.window); // The previous file's window, when one is handed on (docs/session.md).
    renderer_.SetSettledWait(options.settled_sleep && options.low_power_pacing ? 2500000 : 0);
    pace_spin_ = std::int64_t{(std::min)(options.pace_spin_us, 2000u)} * 10; pace_floor_ = pace_spin_ + pace_spin_ / 2;
    renderer_.SetWarmPerception(options.warm_perception);
    renderer_.SetMessageThread(options.message_thread); // The window on a thread of its own (docs/session.md, "The window thread").
    renderer_.SetParentWindow(options.parent_window); // Embedding (API 12): a child of the host's window.
    renderer_.SetTextureOutput(options.texture_width != 0, options.texture_width, options.texture_height, options.texture_format); // API 13, 14.
    display_width_ = width; display_height_ = height;
    renderer_.SetInputIsolation(options.isolate_input); // Before the window exists: its thread filters input from the first message.
    renderer_.EnableHud((options.modules & kModuleHud) != 0); renderer_.AllowNeural((options.modules & kModuleNeuralUpscale) != 0);
    renderer_.SetMusicMode(music_);
    if (warm_ && warm_->gpu.Live()) ++warm_->reuse.gpu_kept;
    if (!renderer_.Prepare(display_width, display_height, surfaces, options.visible, reorder, options.activate, crop_width, crop_height,
                           described.sample_aspect)) {
        Fail(6, renderer_.LastError()); return false;
    }
    if (warm_ && renderer_.KeptSlots()) ++warm_->reuse.slots_kept;
    lap(prepare_times_.renderer);
    renderer_.ConfigurePump(Pump, this);
    renderer_.SetInputIsolation(options.isolate_input); // Automated runs: scripted input only.
    duration_ = (std::max)(video_.duration_100ns, audio_.duration_100ns);
    if (audio_.id) {
        // A layout the platform cannot decode (7.1 AAC on Windows' AAC decoder, HE-AAC, ...) drops the track instead of
        // the playback: its packets are returned undecoded and the endpoint's silence clocks the video.
        codec::AacConfig config{};
        const bool opus = audio_.codec == demux::Codec::Opus;
        if (opus) { config.channels = audio_.channels; config.sample_rate = 48000; config.channel_mask = audio_.channels == 1 ? 0x4u : 0x3u; }
        const auto parsed = opus ? (audio_.channels <= 2 ? codec::AacConfigStatus::Ready : codec::AacConfigStatus::Unsupported) : codec::ParseAacConfig({asc_, asc_bytes_}, config);
        routing_.source_channels = config.channels; routing_.source_mask = config.channel_mask;
        if (parsed != codec::AacConfigStatus::Ready || config.channels > Win32AacDecoder::kPlatformChannels) {
            routing_.status = AudioStatus::UnsupportedLayout; routing_.error = Win32AacDecoder::kUnsupportedLayout;
            ignored_audio_ = audio_.id; audio_ = {};
        } else routing_.status = AudioStatus::Ready;
    }
    seekable_ = !parser_.Fragmented(); // Classic sample tables give random access without I/O.
    renderer_.ConfigureHud(duration_, options.test_hud || options.test_resize, HandleHud, this);
    renderer_.ConfigureKeys(options.key_handler, options.key_context);
    dispatcher_.Configure(HandleIntent, this); dispatcher_.Converse(&conversation_);
    conversation_.Reset(); // Nothing said about another file carries over.
    // The command bar is optional like the subtitles: without its buffers or its text thread, `/` does nothing.
    if (options.modules & kModuleHud) { // From the HUD module's region.
        auto& hud = ModuleArena(kModuleHud);
        if (renderer_.PreparePalette(hud)) renderer_.ConfigurePalette(HandlePaletteSubmit, HandlePalettePreview, this);
        (void)renderer_.PrepareStatus(hud); // Optional too: without it there are no status cues.
    }
    // Perception on the tracker's grids (the objects "next target" moves between, 2026-10-31): optional, from the telemetry
    // module's region.
    if (options.modules & kModuleTelemetry) {
        (void)renderer_.PrepareTargets(ModuleArena(kModuleTelemetry));
        (void)renderer_.PrepareSensing(ModuleArena(kModuleTelemetry)); // The event camera and its virtual IMU (2026-11-03).
    }
    renderer_.SetPicture(options.edge_directed, options.sharpness);
    renderer_.SetQualityMode(options.neural ? render::QualityMode::Neural : options.quality);
    // The paused inspection test compares pictures bit for bit: exact tile reuse (its output is a full evaluation's).
    if (options.test_paused_quality) renderer_.SetNeuralReuse(render::NeuralUpscalePass::TemporalReuse::Exact);
    // Subtitles are optional: any failure here disables them (SubtitleState() == Disabled) and playback proceeds.
    // Every source is a channel of one track (docs/tracks.md): the sidecar first, then each embedded text track.
    std::string_view sources[kMaxTracks]; unsigned source_count = 0; int sidecar = -1;
    auto& subtitle_arena = ModuleArena(kModuleSubtitles); // Used only when the module is enabled (module_used_).
    if ((options.modules & kModuleSubtitles) && (options.subtitle_path || options.test_subtitles || options.test_subtitle_overflow)) {
        std::string_view text; subtitle_state_ = render::SubtitleStatus::Disabled;
        if (options.test_subtitles) {
            text = "WEBVTT\n\n00:00.100 --> 00:01.300\nThe quiet between the frames.\n\n00:01.400 --> 00:02.200\nالعربية · עברית · हिन्दी\n\n00:02.300 --> 00:03.100\n日本語 · 中文 · ไทย\n";
        } else if (options.test_subtitle_overflow) {
            // 4200 distinct CJK ideographs (U+6000...), 70 per cue over 60 cues of 50 ms, and a common phrase in every
            // cue: twice the atlas capacity, paged through it while playing. Every ideograph must be drawn as itself.
            constexpr unsigned cues = 60, per_cue = 70;
            auto* buffer = static_cast<char*>(subtitle_arena.Allocate(cues * (per_cue * 3 + 96) + 16));
            if (buffer) {
                std::size_t n = 0; const auto put = [&](const char* s) { while (*s) buffer[n++] = *s++; };
                put("WEBVTT\n\n");
                for (unsigned c = 0; c < cues; ++c) {
                    char timing[64]{}; const unsigned start = c * 50, end = start + 50;
                    const int w = std::snprintf(timing,sizeof(timing),"00:%02u.%03u --> 00:%02u.%03u\n",start/1000,start%1000,end/1000,end%1000);
                    for (int k = 0; k < w; ++k) buffer[n++] = timing[k];
                    put("\xe5\xad\x97\xe5\xb9\x95 "); // 字幕, used by every cue.
                    for (unsigned k = 0; k < per_cue; ++k) {
                        const unsigned scalar = 0x6000 + c * per_cue + k; // U+6000..U+7067: distinct, apart from 字 and 幕.
                        buffer[n++] = static_cast<char>(0xe0 | (scalar >> 12)); buffer[n++] = static_cast<char>(0x80 | ((scalar >> 6) & 63));
                        buffer[n++] = static_cast<char>(0x80 | (scalar & 63));
                    }
                    put("\n\n");
                }
                text = {buffer, n};
            }
        } else {
            HANDLE file = CreateFileW(options.subtitle_path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            LARGE_INTEGER bytes{};
            if (file == INVALID_HANDLE_VALUE) subtitle_error_ = HRESULT_FROM_WIN32(GetLastError());
            else {
                const bool bounded = GetFileSizeEx(file,&bytes) && bytes.QuadPart > 0 && bytes.QuadPart <= 8*1024*1024;
                auto* buffer = bounded ? static_cast<char*>(subtitle_arena.Allocate(static_cast<std::size_t>(bytes.QuadPart))) : nullptr;
                DWORD read = 0;
                const bool loaded = buffer && ReadFile(file,buffer,static_cast<DWORD>(bytes.QuadPart),&read,nullptr) && read == bytes.QuadPart;
                CloseHandle(file);
                if (loaded) text = {buffer,read}; else subtitle_error_ = bounded ? E_FAIL : HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            }
        }
        if (!text.empty() && track_count_ < kMaxTracks) {
            sidecar = static_cast<int>(track_count_); sources[source_count++] = text;
            auto& m = tracks_[track_count_]; parser_index_[track_count_++] = 0xff;
            m = {}; m.id = 0; m.kind = TrackKind::Subtitle; m.use = TrackUse::Selectable; m.slot = 0; m.enabled = true;
            // Its language: given, else the name's last suffix before the extension ("film.he.srt", "film.heb.vtt").
            ai::CanonicalLanguage(options.sidecar_language, m.language);
            if (!m.language[0] && options.subtitle_path) {
                const std::wstring_view file{options.subtitle_path};
                const auto dot = file.rfind(L'.'), before = dot == file.npos || dot == 0 ? file.npos : file.rfind(L'.', dot - 1);
                if (before != file.npos && dot - before - 1 >= 2 && dot - before - 1 <= 3) {
                    char suffix[4]{}; bool ascii = true;
                    for (std::size_t k = 0; k < dot - before - 1; ++k) { const auto c = file[before + 1 + k]; ascii = ascii && c < 128; suffix[k] = static_cast<char>(c); }
                    if (ascii) ai::CanonicalLanguage(suffix, m.language);
                }
            }
        }
    }
    // Embedded timed text, read now (local files only: see ImportText).
    const auto importing = Now();
    for (unsigned c = 0; (options.modules & kModuleSubtitles) && c < track_count_ && source_count < subtitle::SubtitleTrack::kMaxChannels; ++c) {
        auto& m = tracks_[c];
        if (m.kind != TrackKind::Subtitle || m.use != TrackUse::Selectable || parser_index_[c] == 0xff) continue;
        const auto text = remote_ ? std::string_view{} : ImportText(path, parser_index_[c]);
        if (text.empty()) { m.use = TrackUse::Undecodable; continue; }
        m.slot = static_cast<std::uint8_t>(source_count); sources[source_count++] = text;
    }
    subtitle_import_ = Now() - importing;
    // API 17, subtitles after open: the sources kept (their text is the arena's), and an attached file's slot: the sidecar's
    // when there is one, else the next channel.
    for (unsigned k = 0; k < source_count && k < subtitle::SubtitleTrack::kMaxChannels; ++k) open_sources_[k] = sources[k];
    open_source_count_ = (std::min)(source_count, subtitle::SubtitleTrack::kMaxChannels);
    attached_slot_ = sidecar >= 0 ? 0u : open_source_count_;
    live_track_.store(&subtitles_, std::memory_order_release); live_search_.store(&search_, std::memory_order_release);
    attach_ready_ = attach_broken_ = false; attach_next_ = 0; attach_choice_.store(false);
    if (source_count) {
        subtitle_state_ = render::SubtitleStatus::Disabled;
        if (!subtitles_.Prepare(subtitle_arena, std::span<const std::string_view>{sources, source_count})) subtitle_error_ = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        else {
            // Quote search: folded cue text and a signature per cue, from the arena. It does not depend on the
            // GPU pipeline below.
            const auto indexing = Now();
            // Concept search (the fallback when a quote is in no cue): each cue's concepts, read once here.
            if (search_.Prepare(subtitle_arena, subtitles_)) (void)concepts_.Prepare(subtitle_arena, search_);
            search_prepare_ = Now() - indexing;
            // The pipeline only reserves storage and starts its shaping thread: timed to show it stays off the startup path.
            const auto begin = Now();
            const bool prepared = renderer_.PrepareSubtitles(subtitle_arena, subtitles_, 64); // 64: rebind-ready (API 17), its own atlas size.
            subtitle_prepare_ = Now() - begin;
            if (!prepared) subtitle_error_ = renderer_.LastError(); else subtitle_state_ = render::SubtitleStatus::Ready;
        }
        // Shown at start: a sidecar asked for; else the language asked for; else the file's default subtitle track.
        char wanted_text[4]{}; ai::CanonicalLanguage(options.subtitle_language, wanted_text);
        int shown = sidecar;
        for (unsigned c = 0; shown < 0 && wanted_text[0] && c < track_count_; ++c)
            if (tracks_[c].kind == TrackKind::Subtitle && tracks_[c].use == TrackUse::Selectable && std::strcmp(tracks_[c].language, wanted_text) == 0) shown = static_cast<int>(c);
        for (unsigned c = 0; shown < 0 && c < track_count_; ++c)
            if (tracks_[c].kind == TrackKind::Subtitle && tracks_[c].use == TrackUse::Selectable && tracks_[c].enabled) shown = static_cast<int>(c);
        if (resume_.known) { // As they were left: a track (the sidecar is 0), or none.
            const auto kept = resume_.entry.subtitle_track;
            int found = kept == kNoTrack ? -1 : -2;
            for (unsigned c = 0; found == -2 && c < track_count_; ++c)
                if (tracks_[c].kind == TrackKind::Subtitle && tracks_[c].use == TrackUse::Selectable && tracks_[c].id == kept) found = static_cast<int>(c);
            if (found != -2) { shown = found; resume_.restored_subtitles = true; }
        }
        subtitles_.Show(shown >= 0 ? 1u << tracks_[shown].slot : 0u);
        subtitle_choice_ = subtitle_last_ = shown >= 0 ? tracks_[shown].id : kNoTrack;
    }
    if ((options.modules & kModuleSubtitles) && !source_count && attached_slot_ < subtitle::SubtitleTrack::kMaxChannels) {
        // No subtitles at open: the pipeline is prepared on the empty track all the same (512 atlas cells: 19 MiB of GPU memory),
        // so that a file attached while playing needs nothing made then.
        (void)renderer_.PrepareSubtitles(subtitle_arena, subtitles_, 512);
    }
    if (options.modules & kModuleSubtitles) {
        // The attach storage: the read buffer and two states, views of the module's region (reserved now, sealed with it).
        constexpr std::size_t kState = 10u << 20;
        void* read = subtitle_arena.Allocate(kMaxAttachBytes + 64, 64);
        void* a = subtitle_arena.Allocate(kState, 64); void* b = subtitle_arena.Allocate(kState, 64);
        attach_ready_ = read && a && b && attach_read_.InitializeView(read, kMaxAttachBytes + 64) && attach_states_[0].InitializeView(a, kState) &&
                        attach_states_[1].InitializeView(b, kState) && attached_slot_ < subtitle::SubtitleTrack::kMaxChannels;
    }
    lap(prepare_times_.subtitles);
    if (options.modules & kModuleChapters) PrepareTimeline(path);
    else { timeline_.Clear(); timeline_.SetDuration(video_.duration_100ns > 0 ? video_.duration_100ns : duration_); PackMarks(); }
    lap(prepare_times_.timeline);
    if (music_) {
        // The sound's pictures, worker to UI thread; no decoder (a warm session's waits for the next video file).
        audio_pictures_ = arena_.Create<core::SpscRingBuffer<audio::AudioPicture, 32>>();
        if (!audio_pictures_) { Fail(2); return false; }
    } else {
    DecoderOptions decode; decode.codec = av1_ ? VideoCodec::Upload : hevc_ ? VideoCodec::Hevc : VideoCodec::H264; decode.ten_bit = ten_bit_;
    decode.width = static_cast<std::uint16_t>(width_); decode.height = static_cast<std::uint16_t>(height_); decode.surfaces = surfaces;
    // Upload/bitstream capacity follows the largest access unit (Annex B start codes plus alignment), not a blanket maximum.
    const std::uint64_t sample = video_.max_sample_bytes ? video_.max_sample_bytes : source_->MaxRangeBytes();
    decode.max_bitstream_bytes = static_cast<std::uint32_t>((std::max)((sample + 3 * 256 + 255) & ~std::uint64_t{127}, std::uint64_t{65536}));
    // A warm session's decoder serves again when it fits this stream (restarted at epoch 1); else it is made again, in its own
    // arena (the file arena is rewound for every file).
    bool kept = false;
    if (warm_ && decoder_.Compatible(renderer_.Device(), renderer_.ConsumerFence(), decode)) {
        kept = decoder_.Restart(1) == DecodeStatus::Ready;
        if (kept) ++warm_->reuse.decoders_kept;
    }
    if (!kept) {
        auto* host = &arena_;
        if (warm_) {
            if (decoder_.Prepared() && decoder_.Close() != DecodeStatus::Ready) { Fail(7, decoder_.LastError()); return false; }
            auto& a = warm_->decoder_arena;
            if (!(a.Statistics().capacity_bytes ? a.Reset() : a.Initialize(2u * 1024 * 1024))) { Fail(7, E_OUTOFMEMORY); return false; }
            host = &a; ++warm_->reuse.decoders_made;
        }
        if (decoder_.Prepare(*host, renderer_.Device(), renderer_.ConsumerFence(), decode) != DecodeStatus::Ready) { Fail(7, decoder_.LastError()); return false; }
    }
    if (!renderer_.Bind(decoder_)) { Fail(7, decoder_.LastError()); return false; }
    }
    if (!remote_ && !file_.DetachPollingThread()) { Fail(8, HRESULT_FROM_WIN32(file_.LastOsError())); return false; }
    lap(prepare_times_.decoder);
    // Voice commands, when asked for: the ring to the UI thread, the detector and the recogniser from the arena, and
    // the voice thread, which prepares the capture (COM belongs to it) and waits for Run. A capture that cannot be
    // opened leaves voice off (Voice().error); it never fails playback.
    // The wake policy (docs/voice.md, "Wake word"): the voice's gate and the one for a host's own recognised text (Hear), which
    // needs no module. A custom name (the SDK's MEDIA_WAKE_CUSTOM) replaces "Avion" in both.
    text_gate_.Configure({16000, options.voice_wake_window_ms, options.voice_wake, options.push_to_talk}, nullptr, nullptr);
    (void)text_gate_.SetWord(options.wake_word ? options.wake_word : "");
    if (options.voice != VoiceInput::Off && (options.modules & kModuleVoice)) {
        auto& voice = ModuleArena(kModuleVoice);
        voice_texts_ = voice.Create<core::SpscRingBuffer<VoiceText, 8>>();
        if (voice_texts_ && voice_vad_.Configure(voice) && ConfigureRecogniser(voice, options)) {
            // Recognised text goes through the wake gate: only what follows "Avion" (or comes in its window) runs.
            wake_gate_.Configure({voice_vad_.Options().sample_rate, options.voice_wake_window_ms, options.voice_wake, options.push_to_talk}, HandleWake, this);
            (void)wake_gate_.SetWord(options.wake_word ? options.wake_word : "");
            voice_gate_.Configure(voice_vad_, &ai::AcousticIntentRecognizer::Hear, &recogniser_, &ai::AvionWakeGate::Handle, &wake_gate_);
            recogniser_.FollowWake(&wake_gate_); // Inside a listening window, short commands are sentences too.
            voice_thread_ = CreateThread(nullptr, 0, VoiceEntry, this, 0, nullptr);
            if (voice_thread_) (void)SetThreadDescription(voice_thread_, L"media voice");
            while (voice_thread_ && !voice_ready_.load()) PrepareWait();
        }
    }
    // Frame signatures (kModuleTelemetry): the ring in the module's region; the DSP's analysis runs for them.
    signatures_ = (options.modules & kModuleTelemetry) ? ModuleArena(kModuleTelemetry).Create<core::SpscRingBuffer<FrameSignature, 64>>() : nullptr;
    renderer_.SetSignatureSink(signatures_ ? OnSignature : nullptr, this); // The host's callback is a telemetry feature too.
    speech_now_.store(-1.0f, std::memory_order_relaxed); band_now_.store(audio::SpectralTracker::kBands, std::memory_order_relaxed);
    band_db_now_.store(-144.0f, std::memory_order_relaxed); notches_now_.store(0, std::memory_order_relaxed); // Until the worker hears.
    dsp_controls_.analysis.store(signatures_ != nullptr, std::memory_order_relaxed);
    // Low-power pacing's handles (docs/session.md, "Low-power pacing"). Without a high-resolution timer (before Windows 10
    // 1803) the thread spins as before: a coarse timer wakes up to 15.6 ms late.
    if (options.low_power_pacing) {
        pace_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        pace_picture_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); pace_fence_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        worker_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        worker_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!pace_timer_ || !pace_picture_ || !pace_fence_ || !worker_timer_ || !worker_wake_) {
            for (HANDLE* h : {&pace_timer_, &pace_picture_, &pace_fence_, &worker_timer_, &worker_wake_}) if (*h) { CloseHandle(*h); *h = nullptr; }
        }
    }
    // Forensic snapshots (2026-11-01): their thread waits for a picture to write (none in music mode); the media's name for theirs.
    {
        snapshot_media_[0] = snapshot_stem_[0] = 0;
        if (path && WideCharToMultiByte(CP_UTF8, 0, path, -1, snapshot_media_, sizeof(snapshot_media_), nullptr, nullptr) <= 0) snapshot_media_[0] = 0;
        const char* name = snapshot_media_;
        for (const char* c = snapshot_media_; *c; ++c) if (*c == '/' || *c == '\\') name = c + 1;
        std::size_t n = std::strlen(name);
        for (std::size_t i = n; i-- > 0;) if (name[i] == '.') { n = i; break; }
        n = (std::min)(n, sizeof(snapshot_stem_) - 1); std::memcpy(snapshot_stem_, name, n); snapshot_stem_[n] = 0;
        SetSnapshotFolder(options.snapshot_folder);
        snapshot_phase_.store(0); snapshot_stop_.store(false); snapshot_done_ = false; snapshots_written_ = snapshot_sequence_ = 0;
        if (!music_ && video_.id && (snapshot_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr) {
            snapshot_thread_ = CreateThread(nullptr, 0, SnapshotEntry, this, 0, nullptr);
            if (!snapshot_thread_) { CloseHandle(snapshot_wake_); snapshot_wake_ = nullptr; }
            else (void)SetThreadDescription(snapshot_thread_, L"media snapshot");
        }
    }
    worker_ = CreateThread(nullptr, 0, WorkerEntry, this, 0, nullptr); if (!worker_) { Fail(8); return false; }
    (void)SetThreadDescription(worker_, L"media worker"); // Names for profilers and the benchmark's per-thread CPU.
    while (!ready_.load() && !stopped_.load()) PrepareWait(); // Preparing only.
    lap(prepare_times_.audio);
    if (error_.load() || !ready_.load()) return false;
    bool sealed = arena_.Seal();
    for (unsigned i = 0; i < kModuleCount; ++i) if (module_used_[i]) sealed = module_arenas_[i].Seal() && sealed; // Regions too.
    prepare_times_.total = Now() - preparing;
    return sealed;
}
DWORD WINAPI PlaybackCoordinator::WorkerEntry(void* context) noexcept { static_cast<PlaybackCoordinator*>(context)->Worker(); return 0; }
DWORD WINAPI PlaybackCoordinator::VoiceEntry(void* context) noexcept { static_cast<PlaybackCoordinator*>(context)->VoiceThread(); return 0; }
DWORD WINAPI PlaybackCoordinator::SnapshotEntry(void* context) noexcept { static_cast<PlaybackCoordinator*>(context)->SnapshotThread(); return 0; }
void PlaybackCoordinator::VoiceThread() noexcept {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool script = options_.voice == VoiceInput::Script;
    bool ready = script ? options_.voice_script != nullptr : SUCCEEDED(com);
    if (!script && ready) {
        platform::windows::CaptureOptions capture; // 16 kHz mono, asked of Windows' converter.
        capture.source = options_.voice == VoiceInput::Loopback ? platform::windows::CaptureSource::Loopback : platform::windows::CaptureSource::Microphone;
        ready = capture_.Prepare(ModuleArena(kModuleVoice), capture) == platform::EndpointStatus::Ready;
        // The templates and features are for 16 kHz; an endpoint that refused the conversion leaves voice off.
        if (!ready || capture_.SampleRate() != audio::FilterbankFeatures::kSampleRate) { voice_error_.store(ready ? E_INVALIDARG : capture_.LastError()); ready = false; }
    }
    voice_ready_.store(ready ? 1 : -1);
    if (ready) {
        while (!start_.load() && !stop_.load() && !voice_stop_.load()) Sleep(1);
        if (script) {
            // Real time: 10 ms of script every 10 ms of wall clock since Run began.
            const std::uint32_t total = options_.voice_script_frames; std::uint32_t fed = 0;
            const auto begin = Now();
            while (fed < total && !voice_stop_.load() && !stop_.load()) {
                const auto due = static_cast<std::uint32_t>((std::min)(static_cast<std::int64_t>(total), (Now() - begin) * 16000 / 10000000));
                if (due > fed) { (void)voice_gate_.Process({options_.voice_script + fed, due - fed}); voice_captured_.store(due); fed = due; }
                Sleep(5);
            }
            voice_script_done_.store(true);
        } else if (capture_.Start() == platform::EndpointStatus::Ready) {
            std::uint64_t taken = 0;
            while (!voice_stop_.load() && !stop_.load()) {
                // The voice thread is not on the playback path: it may wait for the endpoint's packets.
                (void)WaitForSingleObject(capture_.Event(), 20);
                if (capture_.Pump() == platform::EndpointStatus::DeviceLost) { voice_error_.store(capture_.LastError()); break; }
                taken += voice_gate_.Pump(capture_); voice_captured_.store(taken);
            }
            (void)capture_.Stop();
        } else voice_error_.store(capture_.LastError());
    }
    while (!voice_stop_.load()) Sleep(1); // Until Close: capture teardown after the caller's measurements.
    capture_.Close();
    if (SUCCEEDED(com)) CoUninitialize();
}
void PlaybackCoordinator::HandleWake(void* context, ai::WakeDecision decision, std::string_view text, const audio::Utterance&) noexcept {
    // Voice thread: the command after the wake word (or in its window), or the name alone, to the UI thread. A full
    // ring drops it (the user repeats it).
    auto& self = *static_cast<PlaybackCoordinator*>(context);
    auto* slot = self.voice_texts_ ? self.voice_texts_->AcquireWriteSlot() : nullptr;
    if (!slot || text.size() > sizeof(slot->text)) { if (slot) (void)self.voice_texts_->CancelWriteSlot(); return; }
    if (!text.empty()) std::memcpy(slot->text, text.data(), text.size()); // Listening carries no text (and no pointer).
    slot->bytes = static_cast<std::uint32_t>(text.size());
    slot->distance = self.recogniser_.Last().distance; slot->decision = static_cast<std::uint32_t>(decision);
    (void)self.voice_texts_->CommitWriteSlot();
    self.WakePresenter(); // A settled pause sleeps: DrainVoice at once.
}
void PlaybackCoordinator::DrainVoice() noexcept {
    while (const auto* heard = voice_texts_ ? voice_texts_->PeekReadSlot() : nullptr) {
        const std::string_view text{heard->text, heard->bytes};
        const auto decision = static_cast<ai::WakeDecision>(heard->decision);
        if (decision == ai::WakeDecision::Listening) {
            // The name alone: a silent cue for as long as the window is open (no spoken reply).
            Status("Avion \xc2\xb7 Listening\xe2\x80\xa6", ui::StatusIcon::Voice, ui::StatusTone::Listening,
                   static_cast<float>(options_.voice_wake_window_ms) / 1000.0f);
            if (voice_results_ < kMaxVoiceResults) {
                auto& r = voice_[voice_results_++]; r = {};
                std::memcpy(r.text, "avion", 6);
                r.wake = decision; r.distance = heard->distance; r.heard = Now() - run_began_;
                r.volume = Volume(); r.muted = Muted(); r.paused = paused_;
            }
            (void)voice_texts_->ReleaseReadSlot();
            continue;
        }
        auto intent = Command(text); // The grammar validates it as it would typed text, and executes it.
        // What was heard, as a cue, unless the command posted its own (a track change names the track).
        const bool own = intent.kind == ai::IntentKind::SelectAudio || intent.kind == ai::IntentKind::SelectSubtitles || intent.kind == ai::IntentKind::SelectLanguage;
        if (!own) {
            char cue[ui::StatusCue::kMaxBytes];
            std::snprintf(cue, sizeof(cue), "\xe2\x80\x9c%.*s\xe2\x80\x9d%s", static_cast<int>(text.size()), text.data(),
                          intent.status == ai::IntentStatus::Ok ? "" : " \xc2\xb7 not done");
            Status(cue, ui::StatusIcon::Voice, intent.status == ai::IntentStatus::Ok ? ui::StatusTone::Info : ui::StatusTone::Warning);
        }
        if (voice_results_ < kMaxVoiceResults) {
            auto& r = voice_[voice_results_++]; r = {};
            std::memcpy(r.text, heard->text, heard->bytes); r.text[heard->bytes] = '\0';
            intent.query = {}; r.intent = intent; r.distance = heard->distance; r.heard = Now() - run_began_; r.wake = decision;
            r.volume = Volume(); r.muted = Muted(); r.paused = paused_;
        }
        (void)voice_texts_->ReleaseReadSlot();
    }
}
PlaybackCoordinator::VoiceState PlaybackCoordinator::Voice() const noexcept {
    VoiceState v; v.active = voice_ready_.load() == 1; v.error = voice_error_.load();
    v.sample_rate = options_.voice == VoiceInput::Script ? 16000 : capture_.SampleRate();
    v.detector = voice_vad_.Stats(); v.gate = voice_gate_.Stats(); v.recogniser = recogniser_.Stats(); v.wake = wake_gate_.Stats();
    v.captured = voice_captured_.load(); v.dropped = capture_.Stats().dropped; v.script_done = voice_script_done_.load();
    return v;
}
int PlaybackCoordinator::ExportTensor(const video::TensorSpec& spec, std::uint32_t kind, const float* fractions, void* out, std::size_t bytes,
                                      render::D3D12Renderer::TensorAnswer& answer, std::uint32_t timeout_ms) noexcept {
    const auto need = video::TensorBytes(spec);
    if (!need || !out || bytes < need || (kind == render::D3D12Renderer::kTensorCustom && !fractions)) return -1;
    render::D3D12Renderer::TensorAsk ask; ask.width = spec.width; ask.height = spec.height; ask.kind = kind;
    if (fractions) { ask.left = fractions[0]; ask.top = fractions[1]; ask.right = fractions[2]; ask.bottom = fractions[3]; }
    if (!renderer_.RequestTensor(ask)) return -1;
    const int got = renderer_.WaitTensor(answer, timeout_ms);
    const bool written = got == 1 && video::ExportTensorMeans(answer.means, spec, out, bytes, answer.color);
    renderer_.FinishTensor();
    return got == 1 ? (written ? 1 : -1) : got;
}
void PlaybackCoordinator::Worker() noexcept {
    // The platform's software decoder (AV1, API 17) runs its own threads at normal priority, flat out while surfaces are free: the
    // thread that hands its pictures to the GPU goes before them (see Run for the presentation thread).
    if (av1_) (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mf = SUCCEEDED(com) ? MFStartup(MF_VERSION, MFSTARTUP_LITE) : com;
    // The endpoint: a warm session's when it is open with the options this file asks for, else opened (and kept) here.
    Win32WasapiAudioSink local_sink;
    auto& sink = warm_ ? warm_->sink : local_sink;
    const auto prepare_sink = [&](const WasapiOptions& o) {
        if (!warm_) return sink.Prepare(arena_, o) == EndpointStatus::Ready;
        auto& w = *warm_; const auto& k = w.sink_options;
        const bool same = w.sink_ready && k.format.sample_rate == o.format.sample_rate && k.format.channels == o.format.channels &&
                          k.format.bytes_per_sample == o.format.bytes_per_sample && k.format.encoding == o.format.encoding && k.ring_frames == o.ring_frames &&
                          k.period_frames == o.period_frames && k.channel_mask == o.channel_mask && k.endpoint_id == o.endpoint_id &&
                          k.allow_conversion == o.allow_conversion;
        if (same) { ++w.reuse.endpoints_kept; return true; }
        if (w.sink_ready) { sink.Close(); w.sink_ready = false; }
        auto& a = w.sink_arena;
        if (!(a.Statistics().capacity_bytes ? a.Reset() : a.Initialize(4u * 1024 * 1024)) || sink.Prepare(a, o) != EndpointStatus::Ready) return false;
        w.sink_options = o; w.sink_ready = true; ++w.reuse.endpoints_made; return true;
    };
    // One decoder per audio track that may be chosen, all prepared now: a track change never creates anything.
    Win32AacDecoder decoders[kMaxAudioTracks];
    Win32AacDecoder* aac = &decoders[first_slot_];
    WasapiOptions audio_options;
    // Decoder first: its output layout (mono, stereo or 5.1) decides the endpoint format.
    const auto prepare_decoder = [](Win32AacDecoder& d, const demux::TrackInfo& t, std::span<const std::byte> asc) {
        return t.codec == demux::Codec::Opus ? d.PrepareOpus(t.channels) : d.Prepare(asc, t.sample_rate, t.channels);
    };
    const bool decoder_ready = FAILED(mf) || !audio_.id || prepare_decoder(*aac, audio_, {asc_, asc_bytes_});
    if (av1_) {
        // The largest access unit when the index knows it (MP4; Matroska once its scan is complete), else 2 MiB.
        const auto* known = parser_.Seekable() ? parser_.Track(video_parser_) : nullptr;
        const auto largest = known && known->max_sample_bytes ? known->max_sample_bytes : video_.max_sample_bytes;
        const auto unit = largest ? (std::max)(largest, 65536u) : 2u << 20;
        if (FAILED(mf) || !av1_decoder_.Prepare(MFVideoFormat_AV1, video_.width, video_.height, ten_bit_, unit)) Fail(40, FAILED(mf) ? mf : av1_decoder_.LastError());
    }
    unsigned channels = audio_.id ? aac->Channels() : 2;
    // The others can replace it at any rate and in any layout the converter takes (docs/tracks.md): the endpoint, the
    // DSP and the clock keep the starting track's format, and another format's PCM is converted to it on its way to the
    // endpoint (audio::PcmConverter, one per slot, prepared now). A track whose decoder cannot be prepared, or whose
    // format cannot be converted, stays listed, Incompatible.
    for (unsigned c = 0; c < track_count_; ++c) {
        auto& m = tracks_[c];
        if (m.kind != demux::TrackKind::Audio || m.use != TrackUse::Selectable) continue;
        auto& other = decoders[m.slot]; const auto& t = audio_tracks_[m.slot];
        const bool start = m.slot == first_slot_;
        const bool prepared = SUCCEEDED(mf) && decoder_ready && audio_.id &&
                              (start || prepare_decoder(other, t, {ascs_[m.slot], asc_sizes_[m.slot]}));
        const bool usable = prepared && converters_[m.slot].Configure(arena_, start ? audio_.sample_rate : t.sample_rate, other.Channels(), audio_.sample_rate, channels);
        if (!usable && !start) { m.use = TrackUse::Incompatible; other.Close(); }
    }
    convert_scratch_ = static_cast<float*>(arena_.Allocate(sizeof(float) * 1024 * audio::PcmConverter::kMaxChannels));
    stretch_scratch_ = static_cast<float*>(arena_.Allocate(sizeof(float) * 1024 * audio::PcmConverter::kMaxChannels));
    if (audio_.id && (!stretch_scratch_ || !stretcher_.Configure(arena_, audio_.sample_rate, channels))) { Fail(9, E_OUTOFMEMORY); }
    bool fold = false;
    if (SUCCEEDED(mf) && decoder_ready && audio_.id) {
        EndpointMix mix{}; (void)Win32WasapiAudioSink::QueryMix(mix);
        routing_.endpoint_channels = mix.channels; routing_.endpoint_mask = mix.channel_mask;
        if (channels > 2) {
            const bool native = mix.channels >= channels && (mix.channel_mask & aac->OutputMask()) == aac->OutputMask();
            fold = options_.surround == SurroundOutput::Stereo || (options_.surround == SurroundOutput::Auto && !native);
            audio_options.allow_conversion = options_.surround == SurroundOutput::Passthrough;
            if (fold) dsp_controls_.spatial.store(true, std::memory_order_relaxed); // Headphones/stereo: binaural by default.
        }
        // A programme rate the endpoint's shared mix does not run at (44.1 kHz audio on a 48 kHz endpoint, as in
        // most downloaded files): Windows resamples. The stream and its clock stay in the programme's own frames.
        if (mix.sample_rate && mix.sample_rate != audio_.sample_rate) audio_options.allow_conversion = true;
    }
    if (audio_.id) {
        audio_options.format.sample_rate = audio_.sample_rate; audio_options.format.channels = static_cast<std::uint16_t>(fold ? 2 : channels);
        routing_.decoded_channels = aac->Channels(); routing_.decoded_mask = aac->DecoderMask();
        routing_.output_channels = audio_options.format.channels; routing_.fold_down = fold;
        routing_.output_mask = fold ? 3u : aac->OutputMask();
    }
    if (fold) fold_scratch_ = static_cast<float*>(arena_.Allocate(sizeof(float) * 1024 * 8));
    if (audio_.id && audio_options.format.sample_rate && audio_options.format.channels) {
        // The pinned loop PCM (docs/seeking.md, "Pinned loop turnaround"): kLoopPcm of endpoint frames.
        loop_pcm_capacity_ = static_cast<unsigned>(static_cast<std::int64_t>(audio_options.format.sample_rate) * kLoopPcm / 10000000);
        loop_pcm_channels_ = audio_options.format.channels;
        loop_pcm_ = static_cast<float*>(arena_.Allocate(sizeof(float) * loop_pcm_capacity_ * loop_pcm_channels_));
        if (!loop_pcm_) loop_pcm_capacity_ = 0;
    }
    // Music mode: what the endpoint is handed is analysed (its rate and channels: after any fold-down).
    if (audio_pictures_ && !analyser_.Configure(arena_, audio_options.format.sample_rate ? audio_options.format.sample_rate : 48000,
                                                audio_options.format.channels ? audio_options.format.channels : 2u)) Fail(9, E_OUTOFMEMORY);
    dsp_.AttachAnalysis(&analysis_tracker_); // Configured with the processor.
    if (FAILED(mf) || !decoder_ready || (fold && !fold_scratch_) || !convert_scratch_ || !prepare_sink(audio_options) ||
        !dsp_.Configure(audio_options.format.sample_rate, audio_.id ? channels : audio_options.format.channels, fold) ||
        !output_gain_.Configure(audio_options.format.sample_rate, audio_options.format.channels)) {
        Fail(9, FAILED(mf) ? mf : !decoder_ready ? aac->LastError() : sink.LastError());
    } else { routing_.converting = sink.Converting(); sink.SetOutputGain(&output_gain_, &dsp_controls_); ready_.store(true); }
    // The audio saccade's locator: the programme's first two channels (front left and right), as decoded.
    if (audio_.id && channels >= 2) (void)locator_.Configure(audio_options.format.sample_rate, channels, 0);
    WorkerPhase(Phase::WorkerStart);
    while (!start_.load() && !stop_.load() && !error_.load()) YieldProcessor();
    // started: the endpoint has been primed and started once; paused: stopped by a UI command.
    bool started = false, paused = false, anchored = false, ever_anchored = false, end = false;
    std::int64_t audio_origin = 0, last_clock = 0, stall_until = 0, started_at = 0; unsigned hiccups = 0;
    bool stalled = false;
    std::uint64_t audio_packets = 0, silence_before = 0;
    // The frames of the units decoded since audio_base: 1024 an AAC unit; an Opus packet's from its duration (API 17). Each unit's
    // time must be audio_base + audio_units / rate (stage 29).
    std::uint64_t audio_units = 0;
    bool have_origin = !audio_.id;
    // Seek state: the playback epoch, and the target below which media is decoded or skipped but never shown.
    std::uint32_t epoch = 1;
    std::int64_t floor = INT64_MIN;
    std::int64_t seek_floor = INT64_MIN; // The epoch's seek target: audio pre-roll trimming ends there.
    bool random_access = false; // After a seek, pictures before the first random-access point are skipped.
    // After a device-loss reposition, audio packets decoded before the loss come round again: they are skipped.
    bool skip_decoded_audio = false;
    // After a device loss while playing, the display floor follows the clock until a frame is shown: pictures already
    // late when decoded stay references only, so video rejoins the audio without late frames.
    bool catch_up = false;
    unsigned lead = 0; std::int64_t lead_since = 0; // Displayable pictures submitted in this epoch, and when the first was.
    SeekWork* work = nullptr; // The current epoch's seek record (none for the start).
    std::int64_t shown_end = INT64_MIN; // End of the latest picture submitted for display in this decode chain.
    // The decode chain's sync sample and the video samples taken since (consecutive in decode order): the next picture a
    // continuation would decode is chain_sync + video_taken, less the packet in hand.
    std::uint32_t chain_sync = 0, video_taken = 0;
    // Scrub preview: this epoch shows its sync sample alone; once that picture is submitted nothing more is read.
    bool previewing = false, preview_shown = false;
    unsigned endpoint_changes = 0; // Sink endpoint changes seen by the last anchor (clock continuity statistics).
    std::int64_t predicted_media = 0, predicted_at = 0; // Last prediction of the anchored clock (0: none since a pause/seek).
    // Audio seek pre-roll. PCM is timed by counting decoder output frames from the first decoded access unit
    // (`audio_base`), as the clock does; the MFT withholds its first output, so output lags input by one unit.
    // After a seek the first `trim_frames` output frames only prime the decoder and are dropped.
    std::int64_t audio_base = 0, trim_frames = 0, produced = 0;
    bool trimming = false;
    // Track changes (docs/tracks.md): the stream decoded now, the PCM frames committed since the epoch's origin (the
    // next one presents at origin + written / rate), and a splice's target, where trimming ends without moving the
    // clock's origin.
    std::uint32_t audio_stream = audio_.id; unsigned audio_slot = first_slot_;
    std::uint64_t written = 0; std::int64_t splice_at = 0; bool splicing = false;
    // Playback speed (docs/audio-dsp.md, "Playback speed"). `committed_frames`: frames handed to the endpoint since the origin, PCM
    // and silence (the device plays them in order: device frame d is committed frame d). The timeline maps committed
    // frames to media time in segments of one speed each; the clock is anchored in the segment the device is in, and
    // again whenever the device enters the next one. While stretching, decoded PCM goes through the stretcher, whose
    // input frame 0 is media `stretch_media` and whose output frame 0 is committed frame `stretch_out`.
    struct Segment { std::uint64_t frame = 0; std::int64_t media = 0; double speed = 1; };
    Segment segments[8]{}; unsigned segment_count = 1;
    float speed = 1.0f; bool stretching = false;
    std::uint64_t committed_frames = 0, stretch_out = 0, anchored_frame = 0; std::int64_t stretch_media = 0;
    AudioClockSample held_sample{}; std::int64_t held_media = 0; bool held_valid = false; // A pause's stop (the resume's anchor).
    audio::TimeStretcher::Change seen_change{};
    int switch_to = -1; std::int64_t switch_requested = 0, switch_waiting_since = 0;
    // The pinned loop PCM's capture (from the first frame at the loop's A, while it plays at 1x on one timeline), and a pinned
    // turn's prefix still to write; `seamless`: the decoded audio after it continues the prefix without the seek's fade-in.
    std::int64_t cap_anchor = INT64_MIN, cap_origin = 0; std::uint64_t cap_start = 0; unsigned cap_frames = 0; std::uint32_t cap_epoch = 0; bool cap_done = false;
    unsigned pinned_left = 0, pinned_pos = 0; bool seamless = false;
    unsigned loudness_frames = 0;
    ToneProbe probe; if (options_.test_tracks) probe.Configure(audio_.sample_rate, fold ? 2u : channels, &probe_);
    SpeedProbe speed_probe; if (options_.test_speed) speed_probe.Configure(audio_.sample_rate, fold ? 2u : channels, &speed_run_, &speed_in_mark_);
    // The decoded track's own rate, and the output path's (the starting track's): decoder frames are counted in the
    // first, endpoint frames (`written`, the clock) in the second. `converter`: set while the playing track is of
    // another format than the output path's; `convert_phase`: where its first output frame lies after its first
    // kept input frame (a splice's target falls between two of the new track's frames).
    unsigned track_rate = audio_.sample_rate;
    const unsigned out_rate = audio_options.format.sample_rate ? audio_options.format.sample_rate : 48000;
    audio::PcmConverter* converter = nullptr; double convert_phase = 0;
    // The audio lead (docs/session.md, "The audio lead", 2026-10-30). Packets come in decode-time order, so while the video
    // packet in hand waits for a decoder surface (the presentation behind it: a slow GPU) the audio behind it could not be read
    // and the endpoint's PCM ran out (52 clock re-anchors in a neural-reuse run). When the PCM queued for the endpoint falls
    // under lead_low, the packets behind are read for their audio: the video ones wait in `backlog`, in order, the one in hand
    // first (parsed or not). One audio packet at a time: after each, the video in hand is tried again (a surface freed meanwhile
    // waits at most one AAC decode: reading on until the PCM was full held the video back at a 4K HDR start, 5 of 40 cold
    // starts dropping pictures, 2 of 40 without reading ahead, 0 of 80 so), until the PCM is back to lead_high (`topping_up`)
    // or the backlog is full.
    constexpr unsigned kBacklog = 32;
    core::MediaPacket backlog[kBacklog]{}; unsigned backlog_head = 0, backlog_count = 0; bool backlog_parsed = false;
    bool video_blocked = false, reading_ahead = false, topping_up = false;
    const unsigned lead_low = out_rate / 8, lead_high = (std::min)(out_rate / 4, audio_options.ring_frames * 3 / 4);
    const auto return_backlog = [&](bool cancel) {
        for (; backlog_count; --backlog_count, backlog_head = (backlog_head + 1) % kBacklog) {
            if (backlog_parsed && cancel) { if (hevc_) hevc_syntax_.Cancel(); else syntax_.Cancel(); }
            backlog_parsed = false;
            if (!parser_.ReturnPacket(backlog[backlog_head])) Fail(11);
        }
        reading_ahead = video_blocked = topping_up = false;
    };
    // A short underrun bridged (same note): its silence (at most bridge_frames) is played with the clock still running, then
    // the clock is anchored again where the sound is, without the hold a lost anchor puts on the pictures.
    const std::uint64_t bridge_frames = (std::max)(std::uint64_t{out_rate} / 50, std::uint64_t{2} * sink.PeriodFrames());
    std::uint32_t gaps_seen = 0; std::int64_t bridging_since = 0; std::int64_t lead_from = 0;
    const auto changes_now = [&] { const auto& m = sink.Recovery(); return m.losses + m.migrations + m.default_changes; };
    // The media time of committed frame `frame`, in the segment it falls in.
    const auto media_at = [&](std::uint64_t frame) {
        const Segment* s = segments;
        for (unsigned i = 1; i < segment_count; ++i) if (segments[i].frame <= frame) s = segments + i;
        return s->media + static_cast<std::int64_t>(static_cast<double>(frame - s->frame) / out_rate * s->speed * 10000000.0);
    };
    const auto add_segment = [&](Segment next) { // Replaces any segment at or after its frame.
        while (segment_count > 1 && segments[segment_count - 1].frame >= next.frame) --segment_count;
        if (segment_count == 1 && segments[0].frame >= next.frame) { segments[0] = next; return; }
        if (segment_count == std::size(segments)) { std::copy(segments + 1, segments + segment_count, segments); --segment_count; }
        segments[segment_count++] = next;
    };
    // A new timeline at `origin` (the start, a seek's first kept frame): committed frame 0 is the origin, at the speed asked.
    const auto begin_timeline = [&](std::int64_t origin) {
        segments[0] = {0, origin, speed}; segment_count = 1; committed_frames = 0; anchored_frame = 0; speed_probe.Restart();
        stretcher_.Reset(); stretching = audio_.id && speed != 1.0f;
        if (stretching) { (void)stretcher_.SetSpeed(speed); stretch_out = 0; stretch_media = origin; }
        seen_change = stretcher_.LastChange();
    };
    const auto trim = [&] {
        const std::int64_t pending = aac->PendingFrames();
        if (trimming) {
            const auto skip = produced + pending <= trim_frames ? pending : (std::max)(trim_frames - produced, std::int64_t{0});
            (void)aac->Skip(static_cast<unsigned>(skip));
            if (aac->Pending()) {
                if (!seamless) aac->FadeIn(track_rate / 400); // A pinned prefix ends where this begins: no fade.
                seamless = false; trimming = false;
                if (converter) converter->Reset(splicing ? convert_phase : 0.0); // Its first output frame: the target.
                if (splicing) splicing = false; // The new track continues the old one's timeline: the origin stays.
                else {
                    // First kept frame, exact to the sample: the new clock origin, entered with a 2.5 ms fade-in.
                    audio_origin = audio_base + (produced + skip) * 10000000 / track_rate; have_origin = true;
                    audio_start_.store(audio_origin, std::memory_order_release);
                    written = 0; probe.Origin(audio_origin); begin_timeline(audio_origin);
                }
            }
        }
        produced += pending;
    };
    // The speed asked for: from the next PCM decoded (through the stretcher), or from the next frame committed (silence).
    const auto apply_speed = [&](float next) {
        if (next == speed || !(next >= audio::TimeStretcher::kMinSpeed && next <= audio::TimeStretcher::kMaxSpeed)) return;
        speed = next;
        if (!have_origin) return; // begin_timeline applies it.
        const bool pcm = audio_.id && (stretching || !(end && aac->Drained() && !aac->Pending()));
        if (!pcm) { add_segment({committed_frames, media_at(committed_frames), next}); return; }
        if (!stretching) {
            if (next == 1.0f) return;
            stretch_out = committed_frames; stretch_media = media_at(committed_frames); stretcher_.Reset(); seen_change = stretcher_.LastChange(); stretching = true;
        }
        (void)stretcher_.SetSpeed(next);
    };
    // Music mode: committed PCM (the endpoint's frames, from committed frame `committed_frames`) into the analyser, and its
    // pictures, stamped with their media time, to the UI thread. A full ring drops the picture (counted).
    const auto analyse = [&](const float* pcm, unsigned frames) {
        if (!audio_pictures_) return;
        for (unsigned done = 0; done < frames;) {
            done += analyser_.Feed(pcm + static_cast<std::size_t>(done) * audio_options.format.channels, frames - done, media_at(committed_frames + done), epoch);
            if (!analyser_.Ready()) continue;
            const auto& picture = analyser_.Take();
            if (auto* slot = audio_pictures_->AcquireWriteSlot()) { *slot = picture; (void)audio_pictures_->CommitWriteSlot(); ++music_run_.published; }
            else ++music_run_.dropped;
        }
    };
    // PCM ready for the endpoint: the decoder's, or what the converter can produce from what it holds, or the stretcher's.
    const auto pending = [&] { return aac->Pending() || (converter && converter->Available() != 0) || (stretching && stretcher_.Available() != 0); };
    // The source's PCM (the converter's, or the decoder's), at most `most` frames into `out`. -1 on failure.
    const auto read_source = [&](float* out, unsigned most) -> int {
        if (converter) {
            while (aac->Pending() && converter->Room()) {
                const unsigned n = (std::min)((std::min)(aac->PendingFrames(), converter->Room()), 1024u);
                const unsigned got = aac->Read({reinterpret_cast<std::byte*>(convert_scratch_), static_cast<std::size_t>(n) * aac->Channels() * sizeof(float)});
                if (got != n) { Fail(14, aac->LastError()); return -1; }
                (void)converter->Push(convert_scratch_, got);
            }
            return static_cast<int>(converter->Pull(out, (std::min)(converter->Available(), most)));
        }
        const unsigned n = (std::min)(aac->PendingFrames(), most);
        if (!n) return 0;
        const unsigned got = aac->Read({reinterpret_cast<std::byte*>(out), static_cast<std::size_t>(n) * channels * sizeof(float)});
        if (got != n) { Fail(14, aac->LastError()); return -1; }
        return static_cast<int>(got);
    };
    // Night mode's meters to the UI, every 100 ms of PCM.
    const auto publish_loudness = [&](unsigned frames) {
        loudness_frames += frames; if (loudness_frames < out_rate / 10) return;
        loudness_frames = 0; const auto& n = dsp_.Night();
        lufs_momentary_.store(n.Meter().Momentary(), std::memory_order_relaxed); lufs_short_.store(n.Meter().ShortTerm(), std::memory_order_relaxed);
        true_peak_.store(n.Output().RecentTruePeak(), std::memory_order_relaxed); night_gain_.store(n.GainDb(), std::memory_order_relaxed);
        limiter_db_.store(n.LimiterDb(), std::memory_order_relaxed);
    };
    // The DSP's analysis for the frame signatures (docs/sdk.md): speech probability, the dominant band and its level.
    const auto publish_analysis = [&]() {
        const auto* a = dsp_.Analysis();
        if (!a || !a->Analyses()) return;
        const auto band = a->Dominant();
        speech_now_.store(a->Speech(), std::memory_order_relaxed); band_now_.store(band, std::memory_order_relaxed);
        band_db_now_.store(a->BandDb(band), std::memory_order_relaxed); notches_now_.store(dsp_.Eq().Notches(), std::memory_order_relaxed);
    };
    // The pinned loop PCM: endpoint frames `pcm` (`frames` of them, the next is committed frame `written`) into the capture.
    const auto capture = [&](const float* pcm, unsigned frames) {
        if (cap_anchor == INT64_MIN || cap_done || !loop_pcm_ || speed != 1.0f || stretching || segment_count != 1 || converter || splicing || !have_origin) return;
        const auto time = [&](std::uint64_t frame) { return audio_origin + static_cast<std::int64_t>(frame * 10000000ull / out_rate); };
        unsigned from = 0;
        if (!cap_frames) {
            if (time(written) > cap_anchor + 10000000 / out_rate) return; // A has gone by in this pass.
            while (from < frames && time(written + from) < cap_anchor) ++from;
            if (from == frames) return;
            cap_origin = time(written + from); cap_start = written + from; cap_epoch = epoch;
        } else if (epoch != cap_epoch || written != cap_start + cap_frames) { cap_frames = 0; return; } // Broken: the next pass.
        const unsigned take = (std::min)(frames - from, loop_pcm_capacity_ - cap_frames);
        std::memcpy(loop_pcm_ + static_cast<std::size_t>(cap_frames) * loop_pcm_channels_, pcm + static_cast<std::size_t>(from) * loop_pcm_channels_,
                    sizeof(float) * take * loop_pcm_channels_);
        cap_frames += take;
        if (cap_frames == loop_pcm_capacity_) {
            cap_done = true;
            loop_pcm_origin_.store(cap_origin, std::memory_order_relaxed); loop_pcm_end_.store(time(cap_start + cap_frames), std::memory_order_relaxed);
            loop_pcm_anchor_.store(cap_anchor, std::memory_order_release);
        }
    };
    // Decoded PCM to the endpoint: at most `most` frames, faded out over them when `fade_out` (a splice's end of the
    // old track). Returns the frames written, 0 when the endpoint had no room (or a converter not yet enough input),
    // -1 on failure.
    const auto write = [&](unsigned most, bool fade_out) -> int {
        if (stretching) {
            // Into the stretcher: the source's PCM (faded on its way in at a splice's end, as the direct path fades it on its
            // way out). Back at 1x the stretcher copies; nothing more goes in, and the direct path resumes once it is empty.
            const bool draining = stretcher_.Passing() && speed == 1.0f;
            int moved = 0;
            while (!draining) {
                unsigned room = (std::min)(stretcher_.Room(), 1024u);
                if (fade_out) room = (std::min)(room, most);
                if (!room) break;
                const int got = read_source(stretch_scratch_, room);
                if (got < 0) return -1;
                if (!got) break;
                if (fade_out)
                    for (int f = 0; f < got; ++f) {
                        const float gain = static_cast<float>(got - 1 - f) / static_cast<float>(got);
                        for (unsigned c = 0; c < channels; ++c) stretch_scratch_[static_cast<std::size_t>(f) * channels + c] *= gain;
                    }
                (void)stretcher_.Push(stretch_scratch_, static_cast<unsigned>(got)); moved += got;
                if (fade_out) break;
            }
            const unsigned ready = stretcher_.Available();
            if (!ready) { if (draining && !stretcher_.Held()) stretching = false; return fade_out ? moved : 0; }
            AudioWriteSpan target{};
            const auto state = sink.TryAcquire((std::min)(ready, fade_out ? ready : most), target);
            if (state == EndpointStatus::WouldBlock) return fade_out ? moved : 0;
            if (state != EndpointStatus::Ready) { Fail(14, sink.LastError()); return -1; }
            auto* pcm = fold ? fold_scratch_ : reinterpret_cast<float*>(target.data);
            const unsigned copied = stretcher_.Pull(pcm, target.frames);
            if (copied != target.frames) { (void)sink.Commit(target.lease, 0); Fail(14, E_FAIL); return -1; }
            if (const auto& c = stretcher_.LastChange(); c.output != seen_change.output || c.input != seen_change.input || c.speed != seen_change.speed) {
                seen_change = c; // A new speed in force from that output frame: the clock follows it there.
                add_segment({stretch_out + c.output, stretch_media + static_cast<std::int64_t>(c.input / out_rate * 10000000.0), c.speed});
            }
            dsp_.Apply(dsp_controls_);
            if (locator_.Configured()) locator_.Process(pcm, copied, -1);
            dsp_.Process(pcm, copied); publish_loudness(copied); publish_analysis();
            if (fold) {
                auto* out = reinterpret_cast<float*>(target.data);
                for (unsigned f = 0; f < copied; ++f) { out[2 * f] = pcm[f * channels]; out[2 * f + 1] = pcm[f * channels + 1]; }
            }
            if (sink.Commit(target.lease, copied) != EndpointStatus::Ready) { Fail(14, sink.LastError()); return -1; }
            if (options_.test_tracks) probe.Feed(reinterpret_cast<const float*>(target.data), copied, written);
            if (options_.test_speed) {
                const Segment* s = segments; for (unsigned i = 1; i < segment_count; ++i) if (segments[i].frame <= committed_frames) s = segments + i;
                speed_probe.Feed(reinterpret_cast<const float*>(target.data), copied, media_at(committed_frames), s->speed);
            }
            analyse(reinterpret_cast<const float*>(target.data), copied);
            written += copied; committed_frames += copied;
            if (draining && !stretcher_.Available() && !stretcher_.Held()) stretching = false;
            return fade_out ? (moved ? moved : static_cast<int>(copied)) : static_cast<int>(copied);
        }
        unsigned ready = 0;
        if (converter) {
            // Another rate or layout: the decoded PCM moves into the track's converter (remixed on the way in), which
            // holds back the few frames its filter needs past each output frame. The decoder is left empty.
            while (aac->Pending() && converter->Room()) {
                const unsigned n = (std::min)((std::min)(aac->PendingFrames(), converter->Room()), 1024u);
                const unsigned got = aac->Read({reinterpret_cast<std::byte*>(convert_scratch_), static_cast<std::size_t>(n) * aac->Channels() * sizeof(float)});
                if (got != n) { Fail(14, aac->LastError()); return -1; }
                (void)converter->Push(convert_scratch_, got);
            }
            ready = converter->Available();
        } else ready = aac->PendingFrames();
        if (!ready) return 0;
        AudioWriteSpan target{};
        const auto state = sink.TryAcquire((std::min)(ready, most), target);
        if (state == EndpointStatus::WouldBlock) return 0;
        if (state != EndpointStatus::Ready) { Fail(14, sink.LastError()); return -1; }
        // Fold-down: decode the surround span into scratch, render it to channels 0/1, keep those two.
        auto* pcm = fold ? fold_scratch_ : reinterpret_cast<float*>(target.data);
        const auto bytes = static_cast<std::size_t>(target.frames) * channels * sizeof(float);
        const unsigned copied = converter ? converter->Pull(pcm, target.frames) :
                                            aac->Read({reinterpret_cast<std::byte*>(pcm), bytes}); // Includes the post-seek fade-in.
        if (copied != target.frames) { (void)sink.Commit(target.lease, 0); Fail(14, aac->LastError()); return -1; }
        if (fade_out)
            for (unsigned f = 0; f < copied; ++f) {
                const float gain = static_cast<float>(copied - 1 - f) / static_cast<float>(copied);
                for (unsigned c = 0; c < channels; ++c) pcm[static_cast<std::size_t>(f) * channels + c] *= gain;
            }
        dsp_.Apply(dsp_controls_);
        if (locator_.Configured()) locator_.Process(pcm, copied, -1);
        dsp_.Process(pcm, copied); publish_loudness(copied); publish_analysis();
        if (fold) {
            auto* out = reinterpret_cast<float*>(target.data);
            for (unsigned f = 0; f < copied; ++f) { out[2 * f] = pcm[f * channels]; out[2 * f + 1] = pcm[f * channels + 1]; }
        }
        capture(reinterpret_cast<const float*>(target.data), copied); // Before Commit: the lease's memory is the endpoint's after it.
        if (sink.Commit(target.lease, copied) != EndpointStatus::Ready) { Fail(14, sink.LastError()); return -1; }
        if (options_.test_tracks) probe.Feed(reinterpret_cast<const float*>(target.data), copied, written);
        if (options_.test_speed) {
            const Segment* s = segments; for (unsigned i = 1; i < segment_count; ++i) if (segments[i].frame <= committed_frames) s = segments + i;
            speed_probe.Feed(reinterpret_cast<const float*>(target.data), copied, media_at(committed_frames), s->speed);
        }
        analyse(reinterpret_cast<const float*>(target.data), copied);
        written += copied; committed_frames += copied;
        return static_cast<int>(copied);
    };
    core::MediaPacket held{}; bool have_packet = false, parsed = false;
    BlockView block{};
    // Low-power pacing (docs/session.md, "Low-power pacing"): a pass that changed nothing waits for the endpoint's period event
    // (it then fills the endpoint at once), a command or a returned picture (WakeWorker), the next clock observation, or 2 ms
    // (10 ms paused). The start before the clock runs, a seek's pre-roll, a device recovery and reads in flight spin as before.
    bool sink_signaled = false;
    const auto idle = [&](bool busy, std::int64_t now) {
        if (busy || !worker_timer_ || (!started && !paused)) { YieldProcessor(); return; }
        const bool running = started && !paused;
        // Paused, the endpoint is stopped and every command and returned picture sets worker_wake_: the timer only bounds the
        // wait (PlaybackOptions::settled_sleep; 10 ms before 2026-11-03, 100 passes a second for nothing).
        std::int64_t wait = running ? 20000 : options_.settled_sleep ? 2500000 : 100000;
        if (running) wait = (std::min)(wait, last_clock + 100000 - now);
        if (wait < 5000) { YieldProcessor(); return; }
        HANDLE handles[3]{worker_timer_, worker_wake_, nullptr}; DWORD count = 2;
        if (running) if (const HANDLE endpoint = sink.Wake()) handles[count++] = endpoint;
        LARGE_INTEGER due{}; due.QuadPart = -wait;
        if (!SetWaitableTimerEx(worker_timer_, &due, 0, nullptr, nullptr, nullptr, 0)) { YieldProcessor(); return; }
        worker_waiting_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (controls_->PeekReadSlot() || surface_returns_->PeekReadSlot()) { worker_waiting_.store(false, std::memory_order_relaxed); return; }
        const auto result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
        worker_waiting_.store(false, std::memory_order_relaxed);
        sink_signaled = count == 3 && result == WAIT_OBJECT_0 + 2;
        ++worker_waits_; worker_waited_ += Now() - now;
    };
    while (!stop_.load() && !error_.load()) {
        worker_beats_.store(worker_beats_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); WorkerPhase(Phase::WorkerLoop);
        // What this pass changes, for the wait at its end.
        const auto pass_frames = committed_frames; const auto pass_audio = audio_packets; const auto pass_epoch = epoch;
        const bool pass_packet = have_packet, pass_started = started, pass_paused = paused, pass_end = end;
        bool busy = false;
        PumpSource();
        // GPU device loss (either thread may notice it first).
        auto gpu = gpu_phase_.load(std::memory_order_acquire);
        if (gpu == kGpuLost) {
            // Stop using the device and drop the DPB: its surfaces went with the device. The decoder then belongs to
            // the presentation thread until it has been rebuilt.
            if (have_packet && held.stream_id == video_.id) {
                if (parsed) { if (hevc_) hevc_syntax_.Cancel(); else syntax_.Cancel(); }
                if (!parser_.ReturnPacket(held)) Fail(11);
                have_packet = parsed = false;
            }
            return_backlog(true);
            platform::SurfaceHandle released[16]{}; unsigned count = 0; // Leases of the lost device: nothing to release.
            if (video_.id && !av1_ && !(hevc_ ? hevc_syntax_.Reset(released, count) : syntax_.Reset(released, count))) { Fail(22); break; }
            if (av1_ && !av1_decoder_.Flush()) { Fail(41, av1_decoder_.LastError()); break; }
            gpu_phase_.store(kGpuParked, std::memory_order_release); gpu = kGpuParked; busy = true;
        } else if (gpu == kGpuResume) {
            // Rebuilt: surface returns still queued name the lost device's leases and are dropped. Decoding restarts in
            // this epoch at the recovery target, from its sync sample; audio played on meanwhile and continues as is.
            while (surface_returns_->PeekReadSlot()) (void)surface_returns_->ReleaseReadSlot();
            if (parser_.Seekable()) {
                if (have_packet) { if (!parser_.ReturnPacket(held)) Fail(11); have_packet = parsed = false; }
                return_backlog(false);
                while (const auto* queued = packets_->PeekReadSlot()) {
                    if (!parser_.ReturnPacket(*queued)) Fail(11);
                    (void)packets_->ReleaseReadSlot();
                }
                const auto target = recovery_target_.load(std::memory_order_relaxed);
                if (target != INT64_MIN) floor = target;
                catch_up = recovery_catch_up_.load(std::memory_order_relaxed);
                // Demux restarts no later than the next undecoded audio packet, so audio stays contiguous; video
                // before the floor is decoded as reference only.
                std::int64_t start = floor == INT64_MIN ? 0 : floor;
                if (audio_.id && audio_packets) start = (std::min)(start, audio_base + static_cast<std::int64_t>(audio_units * 10000000 / track_rate));
                if (error_.load() || !parser_.Reposition(start, kAudioPreroll)) { Fail(33); break; }
                end = false; input_done_.store(false); skip_decoded_audio = audio_packets != 0; shown_end = INT64_MIN;
                chain_sync = video_.id ? parser_.SyncSample(video_parser_, start) : 0; video_taken = 0; preview_shown = false;
            }
            // Pictures before the next random-access point would reference the lost surfaces.
            random_access = true;
            gpu_phase_.store(kGpuRunning, std::memory_order_release); gpu = kGpuRunning; busy = true;
        }
        const bool gpu_live = gpu == kGpuRunning;
        for (unsigned i = 0; gpu_live && i < 32; ++i) {
            const auto* returned = surface_returns_->PeekReadSlot(); if (!returned) break;
            D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS stats{};
            const auto status = decoder_.Statistics({returned->id, returned->generation}, stats);
            if (status == DecodeStatus::WouldBlock) break;
            if (status == DecodeStatus::DeviceLost) { LoseGpu(2, decoder_.LastError()); break; }
            if (status != DecodeStatus::Ready || stats.Status != D3D12_VIDEO_DECODE_STATUS_OK || stats.NumMacroblocksAffected != 0) {
                Fail(28, status == DecodeStatus::Ready ? static_cast<HRESULT>(stats.Status) : decoder_.LastError()); break;
            }
            if (!decoder_.ReturnSurface(*returned)) { Fail(10); break; }
            (void)surface_returns_->ReleaseReadSlot(); validated_frames_.fetch_add(1); busy = true;
        }
        if (error_.load()) break;
        while (const auto* returned = packet_returns_->PeekReadSlot()) {
            if (!parser_.ReturnPacket(*returned)) Fail(11);
            (void)packet_returns_->ReleaseReadSlot(); busy = true;
        }
        // UI commands. Pause stops the endpoint (device position is preserved) and invalidates the clock,
        // which holds video; resume restarts it and the next precise observation re-anchors below.
        WorkerPhase(Phase::WorkerControls);
        ControlCommand seek{}; bool have_seek = false;
        while (const auto* command = hold_controls_.load(std::memory_order_acquire) ? nullptr : controls_->PeekReadSlot()) {
            // A seek needs the decoder, which the presentation thread owns during a device recovery: it waits.
            if (command->kind == ControlCommand::Kind::Seek && !gpu_live) break;
            const auto control = *command;
            (void)controls_->ReleaseReadSlot(); busy = true;
            if (control.kind == ControlCommand::Kind::Seek) {
                // Coalescing: of the seeks drained together only the newest is applied; earlier ones never start a
                // decode. Their epochs are already flushed from presentation by the UI.
                if (have_seek) superseded_seeks_.fetch_add(1);
                seek = control; have_seek = true;
                continue;
            }
            if (control.kind == ControlCommand::Kind::Speed) { apply_speed(static_cast<float>(control.epoch) / 1000.0f); continue; }
            if (control.kind == ControlCommand::Kind::LoopAnchor) { // A new loop (or none): its PCM is captured again.
                cap_anchor = control.target_100ns; cap_frames = 0; cap_done = false;
                loop_pcm_anchor_.store(INT64_MIN, std::memory_order_release);
                continue;
            }
            if (control.kind == ControlCommand::Kind::SelectAudio) { // The newest one counts.
                switch_to = static_cast<int>(control.epoch); switch_requested = control.target_100ns;
                continue;
            }
            const bool pause = control.kind == ControlCommand::Kind::Pause;
            if (pause == paused) continue;
            paused = pause;
            // Where the device stops (the anchored clock's position and media time), for the anchor at the resume.
            if (pause) held_valid = started && anchored && sink.Clock(held_sample) == EndpointStatus::Ready && held_sample.precise;
            if (pause && held_valid) {
                const double seconds = static_cast<double>(held_sample.position) / static_cast<double>(held_sample.frequency) +
                                       (static_cast<double>(sink.EndpointOffsetFrames()) - static_cast<double>(sink.SilenceFrames())) / audio_options.format.sample_rate;
                held_media = media_at(static_cast<std::uint64_t>((std::max)(seconds * out_rate, 0.0)));
                held_valid = segment_count == 1 && !stretching; // One timeline at one speed: the device's position names the media time.
            }
            if (started && (pause ? sink.Stop() : sink.Start()) != EndpointStatus::Ready) { Fail(16, sink.LastError()); break; }
            if (pause) { clock_.Invalidate(); anchored = false; predicted_at = 0; }
            else if (started && held_valid) {
                // Resumed: the device plays on from where it stopped, now. Anchored there at once, the clock does not wait the
                // 10-20 ms until the device reports a precise position (pictures due meanwhile were dropped as late: a 60 fps
                // picture due within 2 ms of the pause, 2026-10-22); the next precise observation disciplines it as usual.
                AudioClockSample now_sample{};
                if (sink.Clock(now_sample) == EndpointStatus::Ready) {
                    AudioClockSample start = held_sample; start.monotonic_time_100ns = Now(); start.generation = now_sample.generation; start.precise = true;
                    anchored = clock_.Reanchor(start, held_media, epoch, speed);
                    if (anchored) {
                        anchored_frame = segments[0].frame; anchored_at_.store(Now(), std::memory_order_release);
                        worker_events_.Record(Now(), core::FlightEvent::Underrun, 0, discontinuities_.fetch_add(1) + 1); ever_anchored = true;
                    }
                }
                held_valid = false;
            }
        }
        // Audio track change (docs/tracks.md). The catalogue entry's fields read here were written before `ready_`.
        WorkerPhase(Phase::WorkerSwitch);
        bool postponed = false;
        if (switch_to >= 0 && static_cast<unsigned>(switch_to) < track_count_ && !error_.load() && audio_.id) do {
            const auto& next = tracks_[switch_to];
            if (next.kind != demux::TrackKind::Audio || next.use != TrackUse::Selectable || next.id == audio_stream) break;
            unsigned from = 0, to = parser_index_[switch_to];
            for (unsigned c = 0; c < track_count_; ++c) if (tracks_[c].kind == demux::TrackKind::Audio && tracks_[c].id == audio_stream) from = parser_index_[c];
            TrackSwitch record{audio_stream, next.id, switch_requested, 0, 0, written, -1, false};
            // A splice needs PCM on this epoch's timeline to continue (also while a previous splice is still trimming);
            // a seek being applied positions every track itself.
            const bool splice = have_origin && !have_seek && parser_.Seekable();
            if (splice && !splicing) {
                // The old track fades out over 5 ms of its own decoded PCM. That needs decoded PCM (the packet in hand
                // is decoded first) and room at the endpoint: the change waits for both, 50 ms at most.
                if (!switch_waiting_since) switch_waiting_since = Now();
                const bool waited = Now() - switch_waiting_since > 500000;
                if (!pending() && have_packet && held.stream_id == audio_stream && !waited) { postponed = true; break; }
                if (pending()) {
                    const int faded = write(out_rate / 200, true);
                    if (faded < 0) break;
                    if (faded == 0 && !waited) { postponed = true; break; }
                }
            }
            switch_waiting_since = 0;
            // The old track's packet in hand goes back unread; its PCM already committed plays out.
            if (have_packet && held.stream_id == audio_stream) { if (!parser_.ReturnPacket(held)) { Fail(11); break; } have_packet = false; }
            if (splice) {
                splice_at = stretching ? stretch_media + static_cast<std::int64_t>(static_cast<double>(stretcher_.Pushed()) / out_rate * 10000000.0) :
                            speed == 1.0f && segment_count == 1 && !segments[0].frame ? audio_origin + static_cast<std::int64_t>(written * 10000000 / out_rate) :
                            media_at(committed_frames);
                if (!parser_.SwitchTrack(from, to, splice_at, kAudioPreroll)) { Fail(35); break; }
                record.at = splice_at; record.frame = written; record.spliced = true;
                clock::ClockEstimate estimate{};
                if (clock_.Estimate(Now(), estimate) == clock::ClockQuery::Ready && estimate.epoch == epoch) record.queued = splice_at - estimate.media_time_100ns;
                splicing = true; trimming = true;
                if (options_.test_tracks) probe.Splice(written);
            } else if (have_seek) {
                if (!parser_.SetEmission(from, false) || !parser_.SetEmission(to, true)) { Fail(35); break; }
                record.at = seek.target_100ns;
            } else {
                // Nothing of this epoch written yet: the new track starts where the epoch does, trimmed as a seek's.
                const auto target = seek_floor == INT64_MIN ? 0 : seek_floor;
                const bool moved = parser_.Seekable() ? parser_.SwitchTrack(from, to, target, kAudioPreroll) :
                                   parser_.SetEmission(from, false) && parser_.SetEmission(to, true);
                if (!moved) { Fail(35); break; }
                record.at = target;
            }
            if (!aac->Flush()) { Fail(19, aac->LastError()); break; }
            aac = &decoders[next.slot]; audio_slot = next.slot; audio_stream = next.id;
            if (!aac->Flush()) { Fail(19, aac->LastError()); break; }
            // The new track's own rate; converted when its rate or layout is not the output path's.
            track_rate = audio_tracks_[next.slot].sample_rate;
            converter = converters_[next.slot].Identity() ? nullptr : &converters_[next.slot];
            if (converter) converter->Reset();
            record.rate = track_rate; record.channels = aac->Channels(); record.converted = converter != nullptr;
            audio_packets = audio_units = 0; produced = trim_frames = 0; skip_decoded_audio = false;
            if (end && !have_seek) { end = false; input_done_.store(false); } // The new track may have more to play.
            record.applied = Now();
            const auto n = switch_count_.load(std::memory_order_relaxed);
            if (n < kMaxSwitches) { switches_[n] = record; switch_count_.store(n + 1, std::memory_order_release); }
            worker_events_.Record(record.applied, core::FlightEvent::TrackSwitched, record.spliced ? 1 : 0, record.to, record.at, record.applied - record.requested);
            cap_frames = 0; cap_done = false; loop_pcm_anchor_.store(INT64_MIN, std::memory_order_release); // Another track's sound.
        } while (false);
        if (!postponed) switch_to = -1;
        if (error_.load()) break;
        WorkerPhase(Phase::WorkerSeek);
        if (have_seek && !error_.load()) do {
            // A pinned loop turn (docs/seeking.md, "Pinned loop turnaround"): the UI shows the pinned pictures from A and asks for
            // the end of them; the endpoint gets the pinned PCM from A first. Without that PCM (another track, another speed),
            // it is an ordinary turn to A.
            const std::int64_t pinned_a = static_cast<std::int64_t>(static_cast<std::uint64_t>(seek.reserved[2]) | static_cast<std::uint64_t>(seek.reserved[3]) << 32);
            const bool pinned_turn = (seek.reserved[1] & 1) && audio_.id && cap_done && pinned_a == cap_anchor && !converter && speed == 1.0f &&
                                     !stretching && seek.target_100ns > cap_origin;
            if ((seek.reserved[1] & 1) && audio_.id && !pinned_turn) seek.target_100ns = pinned_a;
            const auto& control = seek;
            // Forward continuation (docs/seeking.md): when the target lies ahead in the group of pictures being decoded
            // (no sync sample between the next video sample and the target, and no picture already read still to be
            // shown), decoding carries on from where it is. The codec state, the DPB and the video packets already read
            // stay: they are the next links of the chain the target needs. Only audio and the clock are repositioned.
            // Decoding must have started since the last repositioning, so that the chain is the one from the sync sample.
            //
            // Queue seek (docs/seeking.md): a forward target among the pictures already submitted for display in this
            // decode chain. Nothing is decoded again: video carries on exactly as it is, the presentation side drops what
            // ends before the target and adopts the rest. The UI asked for it (reserved[0] bit 0) and, when its landing
            // picture was already converted (bit 1), the adoption is certain and must be kept.
            //
            // Scrub preview (docs/seeking.md): a scrub seek (bit 2) whose exact frame needs more than kPreviewReferences
            // pictures decoded before it, from the sync sample or from where this chain would continue, decodes and shows
            // that sync sample alone, while the drag moves backward (bit 3 clear). A forward drag is left exact: queue seeks and
            // continuations serve it, and a preview would leave the decoder holding the sync sample alone, so every later
            // target of that group of pictures would restart from it (measured: fewer frames in the forward half).
            std::uint32_t resumed = 0;
            const auto ask = control.reserved[0];
            const bool adopted = video_.id && (ask & 1) && gpu_live && !random_access &&
                                 ((ask & 2) || (control.target_100ns >= floor && control.target_100ns < shown_end)) &&
                                 parser_.SeekOthers(video_parser_, control.target_100ns, control.epoch, kAudioPreroll);
            bool preview = false;
            if (!adopted && (ask & 4) && gpu_live && video_.id) {
                const auto sync = parser_.SyncSample(video_parser_, control.target_100ns);
                if (sync != UINT32_MAX && !(ask & 8)) {
                    auto cost = parser_.DecodeDistance(video_parser_, sync, control.target_100ns, kPreviewReferences);
                    const auto next = chain_sync + video_taken - (have_packet && held.stream_id == video_.id ? 1u : 0u) - backlog_count;
                    if (continuation_.load(std::memory_order_relaxed) && !random_access && sync == chain_sync && next > sync)
                        cost = (std::min)(cost, parser_.DecodeDistance(video_parser_, next, control.target_100ns, kPreviewReferences));
                    preview = cost > kPreviewReferences;
                }
            }
            const bool continued = video_.id && !adopted && !preview && continuation_.load(std::memory_order_relaxed) && gpu_live && !random_access &&
                                   parser_.Continue(video_parser_, control.target_100ns, control.epoch, kAudioPreroll, resumed);
            if (continued || adopted) {
                // The packet in hand: video continues into the new epoch; audio belongs to the old position. Queued
                // packets are sorted the same way as they are taken (below).
                if (have_packet && held.stream_id != video_.id) { if (!parser_.ReturnPacket(held)) Fail(11); have_packet = parsed = false; }
                if (have_packet) held.playback_epoch = control.epoch;
                reading_ahead = false; // The backlog's video continues too (relabelled as it is taken).
                (adopted ? adopted_seeks_ : continued_seeks_).fetch_add(1, std::memory_order_relaxed);
            } else {
                // 1. Earlier-epoch input returns its block leases: the held packet and anything already queued.
                if (have_packet) {
                    if (parsed) { if (hevc_) hevc_syntax_.Cancel(); else syntax_.Cancel(); }
                    if (!parser_.ReturnPacket(held)) Fail(11);
                    have_packet = parsed = false;
                }
                return_backlog(true);
                while (const auto* queued = packets_->PeekReadSlot()) {
                    if (!parser_.ReturnPacket(*queued)) Fail(11);
                    (void)packets_->ReleaseReadSlot();
                }
                // A classic file is seekable once its scan reached the end; finishing it publishes no packets.
                const auto deadline = GetTickCount64() + 2000;
                while (!parser_.Seekable() && !error_.load() && GetTickCount64() < deadline) {
                    PumpSource();
                    if (parser_.Step(*packets_) == demux::ParseStatus::Failed) Fail(17, static_cast<HRESULT>(parser_.Diagnostic().error));
                }
                // 2. Demux: sync-sample lookup, new epoch. 3. Codec: every DPB hold returns to the decoder.
                if (error_.load() || !parser_.Seek(control.target_100ns, control.epoch, kAudioPreroll)) { Fail(33); break; }
                chain_sync = video_.id ? parser_.SyncSample(video_parser_, control.target_100ns) : 0; video_taken = 0;
                platform::SurfaceHandle released[16]{}; unsigned count = 0;
                if (video_.id && !av1_ && !(hevc_ ? hevc_syntax_.Reset(released, count) : syntax_.Reset(released, count))) { Fail(22); break; }
                if (av1_ && !av1_decoder_.Flush()) { Fail(41, av1_decoder_.LastError()); break; }
                for (unsigned i = 0; i != count; ++i) if (!decoder_.ReleaseReference(released[i])) Fail(23);
            }
            if (video_.id && !decoder_.SetEpoch(control.epoch)) { Fail(33); break; }
            if (const auto n = work_count_.load(std::memory_order_relaxed); n < kMaxSeekWork) {
                work_[n] = {control.epoch, continued, resumed, 0, 0, adopted, (ask & 2) != 0, preview}; work = &work_[n]; work_count_.store(n + 1, std::memory_order_release);
            } else work = nullptr;
            // 4. Audio: drop decoded and queued PCM, restart the endpoint position; the clock waits for the new
            // timeline, anchored at the first kept PCM frame (the target itself) or, without audio, the target.
            if (audio_.id && !aac->Flush()) { Fail(19, aac->LastError()); break; }
            if (converter) converter->Reset();
            silence_before += sink.SilenceFrames();
            if (sink.Reset() != EndpointStatus::Ready) { Fail(16, sink.LastError()); break; }
            dsp_.Reset(); ++dsp_resets_; analyser_.Reset(); locator_.Reset();
            clock_.Invalidate(); anchored = started = end = false; predicted_at = 0;
            epoch = control.epoch; floor = seek_floor = control.target_100ns < 0 ? 0 : control.target_100ns; random_access = !continued && !adopted; // Both keep the decode chain.
            skip_decoded_audio = catch_up = false; lead = 0; lead_since = 0;
            audio_packets = audio_units = 0; have_origin = !audio_.id; audio_origin = audio_.id ? 0 : floor;
            begin_timeline(audio_origin); // The origin of an audio track's epoch is its first kept frame: made again then.
            produced = trim_frames = 0; trimming = audio_.id != 0; splicing = false; written = 0;
            pinned_left = 0; seamless = false;
            if (pinned_turn) {
                // The pinned PCM from A plays first (as captured: after the DSP, before the volume); the decoded audio joins it
                // at its end, as a splice does, keeping the origin at A and without the seek's fade-in.
                const auto prefix = (std::min)(static_cast<std::uint64_t>((control.target_100ns - cap_origin) * out_rate / 10000000), std::uint64_t{cap_frames});
                have_origin = true; audio_origin = cap_origin; probe.Origin(audio_origin); begin_timeline(audio_origin);
                audio_start_.store(audio_origin, std::memory_order_release);
                pinned_left = static_cast<unsigned>(prefix); pinned_pos = 0;
                splice_at = audio_origin + static_cast<std::int64_t>(prefix * 10000000 / out_rate); splicing = true; seamless = true;
            }
            input_done_.store(false); audio_end_.store(0); held_valid = false; // A pause's stop is not this timeline's.
            if (!adopted) { shown_end = INT64_MIN; chain_epoch_.store(epoch, std::memory_order_relaxed); }
            adopted_epoch_.store(adopted ? epoch : 0, std::memory_order_relaxed);
            previewing = preview; preview_shown = false;
            preview_epochs_[epoch % 16].store(preview ? epoch : 0, std::memory_order_relaxed);
            if (preview) preview_seeks_.fetch_add(1, std::memory_order_relaxed);
            worker_epoch_.store(epoch, std::memory_order_release);
            applied_seeks_.fetch_add(1);
            worker_events_.Record(Now(), core::FlightEvent::SeekApplied, static_cast<std::uint16_t>((continued ? 1u : 0u) | (adopted ? 2u : 0u) | (preview ? 4u : 0u)),
                                  control.epoch, control.target_100ns);
        } while (false);
        if (error_.load()) break;
        WorkerPhase(Phase::WorkerMedia);
        const auto now = Now();
        if (options_.stall_audio_ms && started && !stalled && now - started_at >= options_.stall_audio_at_ms * 10000ll) {
            stall_until = now + options_.stall_audio_ms * 10000ll; stalled = true;
        }
        const bool stalling = now < stall_until; // Test hook: PCM is withheld, so the endpoint must underrun.
        if (options_.test_hiccup && started && !paused && hiccups < 2 && now - started_at >= (hiccups ? 50000000ll : 20000000ll)) {
            sink.InjectGap(out_rate * (hiccups ? 15u : 10u) / 1000u); ++hiccups; // Test hook: a 10 ms, then a 15 ms gap.
        }
        if (started && !paused) {
            if (audio_fault_.load(std::memory_order_relaxed)) { // Fault test (InjectAudioFault).
                const auto fault = audio_fault_.exchange(0, std::memory_order_acquire);
                sink.InjectInvalidation(static_cast<HRESULT>(static_cast<std::uint32_t>(fault)), static_cast<std::uint32_t>(fault >> 32));
            }
            const auto pumped = sink.Pump(sink_signaled); sink_signaled = false;
            if (pumped != EndpointStatus::Ready && pumped != EndpointStatus::WouldBlock) { Fail(12, sink.LastError()); break; }
            const auto& recovered = sink.Recovery(); // Endpoint changes, for the UI's cue.
            const unsigned moves = recovered.losses + recovered.migrations + recovered.default_changes;
            if (moves != endpoint_moves_.load(std::memory_order_relaxed)) worker_events_.Record(now, core::FlightEvent::AudioMoved, 0, moves);
            endpoint_moves_.store(moves, std::memory_order_relaxed);
            if (now - last_clock >= 100000) {
                AudioClockSample sample{};
                if (sink.Clock(sample) != EndpointStatus::Ready) { Fail(12, sink.LastError()); break; }
                silence_frames_.store(silence_before + sink.SilenceFrames());
                // Endpoint-inserted underrun silence occupies device time but carries no media, so it is subtracted. The
                // sink reports precise tuples only after that silence has been played. Frames played by endpoints the
                // stream has left (a lost device, the null endpoint) are its offset.
                const double seconds = static_cast<double>(sample.position) / static_cast<double>(sample.frequency) +
                                       (static_cast<double>(sink.EndpointOffsetFrames()) - static_cast<double>(sink.SilenceFrames())) /
                                       audio_options.format.sample_rate;
                // The segment the device is in (earlier ones are done with); a later one than the anchor's: anchor again.
                const double device_frame = seconds * out_rate;
                unsigned in_segment = 0;
                for (unsigned i = 1; i < segment_count; ++i) if (static_cast<double>(segments[i].frame) <= device_frame) in_segment = i;
                if (in_segment) { std::copy(segments + in_segment, segments + segment_count, segments); segment_count -= in_segment; }
                const bool speed_change = anchored && segments[0].frame != anchored_frame;
                if (sample.precise && sample.position && (!anchored || speed_change)) {
                    const auto& s = segments[0];
                    const auto media = s.media + static_cast<std::int64_t>((seconds - static_cast<double>(s.frame) / out_rate) * s.speed * 10000000.0);
                    const auto& moved = sink.Recovery();
                    const unsigned changes = moved.losses + moved.migrations + moved.default_changes;
                    if (changes != endpoint_changes && predicted_at && !speed_change) {
                        // First anchor on a new endpoint, against where the old endpoint's clock (its last prediction,
                        // run on at 1x) says playback would be now, and against what it had already reached.
                        const auto predicted = predicted_media + static_cast<std::int64_t>(static_cast<double>(sample.monotonic_time_100ns - predicted_at) * s.speed);
                        auto& c = continuity_; ++c.handovers;
                        c.skip = (std::max)(c.skip, media - predicted);
                        c.hold = (std::max)(c.hold, predicted - (std::max)(media, predicted_media));
                        c.repeat = (std::max)(c.repeat, predicted_media - media);
                    }
                    anchored = clock_.Reanchor(sample, media, epoch, s.speed);
                    if (anchored) { endpoint_changes = changes; anchored_frame = s.frame; gaps_seen = sink.Gaps(); bridging_since = 0; }
                    if (anchored) anchored_at_.store(Now(), std::memory_order_release);
                    if (anchored && ever_anchored && !speed_change) { // A speed's start is no discontinuity.
                        worker_events_.Record(Now(), core::FlightEvent::Underrun, 0, discontinuities_.fetch_add(1) + 1);
                    }
                    ever_anchored = ever_anchored || anchored;
                } else if (anchored && sink.Gaps() != gaps_seen && changes_now() == endpoint_changes && sink.GapFrames() <= bridge_frames &&
                           (!bridging_since || now - bridging_since < 1500000)) {
                    // A short underrun: the clock runs on through its silence (no hold on the pictures); once the silence has been
                    // played, anchored again where the sound is (behind the run-on clock by at most the silence).
                    if (!bridging_since) bridging_since = now;
                    if (!sink.InGap() && sample.precise && sample.position) {
                        const auto& s = segments[0];
                        const auto media = s.media + static_cast<std::int64_t>((seconds - static_cast<double>(s.frame) / out_rate) * s.speed * 10000000.0);
                        if (clock_.Reanchor(sample, media, epoch, s.speed)) {
                            gaps_seen = sink.Gaps(); bridging_since = 0; anchored_frame = s.frame; bridged_gaps_.fetch_add(1, std::memory_order_relaxed);
                            worker_events_.Record(Now(), core::FlightEvent::Underrun, 1, bridged_gaps_.load(std::memory_order_relaxed));
                        }
                    }
                } else if (anchored) {
                    bridging_since = 0;
                    // The anchored clock's prediction at this observation, kept for the continuity statistics: a
                    // generation change (a new endpoint) invalidates the clock inside Observe. Such a sample is the new
                    // endpoint's: the old clock extrapolated to it would run past the instant the old endpoint stopped,
                    // so it is not kept as the old endpoint's last prediction.
                    clock::ClockEstimate estimate{};
                    const bool predicted = clock_.Estimate(sample.monotonic_time_100ns, estimate) == clock::ClockQuery::Ready && estimate.epoch == epoch;
                    // Underrun (generation change), a starved observation gap or repeated outliers: not fatal.
                    // Video holds while the clock is unanchored; the next precise sample re-anchors it.
                    if (clock_.Observe(sample) == clock::ClockUpdate::NeedsAnchor) anchored = false;
                    else if (predicted) { predicted_media = estimate.media_time_100ns; predicted_at = sample.monotonic_time_100ns; }
                }
                last_clock = now;
            }
        }
        // The end of a converted track: the frames its filter held back are completed with silence and played.
        if (converter && end && aac->Drained() && !aac->Pending() && !converter->Finished()) converter->Finish();
        if (pinned_left && !stalling) {
            // A pinned turn's prefix (already through the DSP): to the endpoint as it has room, before any decoded PCM.
            AudioWriteSpan span{};
            const auto state = sink.TryAcquire((std::min)(pinned_left, 1024u), span);
            if (state == EndpointStatus::Ready) {
                std::memcpy(span.data, loop_pcm_ + static_cast<std::size_t>(pinned_pos) * loop_pcm_channels_, sizeof(float) * span.frames * loop_pcm_channels_);
                if (sink.Commit(span.lease, span.frames) != EndpointStatus::Ready) { Fail(15); break; }
                written += span.frames; committed_frames += span.frames; pinned_pos += span.frames; pinned_left -= span.frames;
            } else if (state != EndpointStatus::WouldBlock) { Fail(14, sink.LastError()); break; }
        } else if (pending() && !stalling && write(1024, false) < 0) break;
        if (end && audio_.id && !aac->Pending() && !aac->Drained()) {
            if (!aac->Drain()) { Fail(19, aac->LastError()); break; }
            trim(); // The final access unit's PCM arrives with the drain.
            audio_frames_.store(aac->DecodedFrames());
        }
        if ((!audio_.id || (end && aac->Drained() && (!converter || converter->Finished()))) && !pending() && !stalling) {
            AudioWriteSpan silence{};
            if (sink.TryAcquire(1024, silence) == EndpointStatus::Ready) {
                std::memset(silence.data, 0, static_cast<std::size_t>(silence.frames) * audio_options.format.channels * 4);
                dsp_.Apply(dsp_controls_);
                if (fold) { // Let delay and filter tails of the surround render finish, then keep the two ears.
                    std::memset(fold_scratch_, 0, static_cast<std::size_t>(silence.frames) * channels * 4);
                    dsp_.Process(fold_scratch_, silence.frames);
                    auto* out = reinterpret_cast<float*>(silence.data);
                    for (unsigned f = 0; f < silence.frames; ++f) { out[2 * f] = fold_scratch_[f * channels]; out[2 * f + 1] = fold_scratch_[f * channels + 1]; }
                } else dsp_.Process(reinterpret_cast<float*>(silence.data), silence.frames);
                if (sink.Commit(silence.lease, silence.frames) != EndpointStatus::Ready) { Fail(15); break; }
                committed_frames += silence.frames;
            }
        }
        // Primed -> Running (pre-roll): the endpoint, and with it the clock, starts or restarts once PCM is queued and the
        // first picture of this epoch has been decoded, converted (tone-mapped) and handed to the swap chain, so a slow
        // first decode (a cold GPU on a 4K IRAP) cannot put video behind the clock; a remote source also waits for its
        // lead. At end of input the picture may never come (no video left), and a picture that cannot be shown must not
        // hold the sound back for ever: both open the gate, the latter after kPrerollLimit.
        const bool waited = lead && Now() - lead_since > kPrerollLimit;
        // The sound comes first (2026-11-04): no picture yet and the AAC in hand cannot be decoded, its PCM queue full. The parser
        // emits in time order, so this is a track whose pictures start later than the sound (an empty edit, `-itsoffset`, a
        // trimmed clip): nothing else can move, the clock starts on the sound and the first picture shows at its own time. It
        // was a dead lock: 0.36 s of PCM queued, the first picture 0.5 s in, never read.
        const bool sound_first = !lead && have_packet && held.stream_id == audio_stream && aac->Pending();
        // A network source, and the platform's software decoder (AV1, API 17: it delivers in bursts, a picture 7 ms after another
        // or 30 ms), start with kRemoteLead pictures decoded.
        const bool primed = !(remote_ || av1_) || music_ || lead > kRemoteLead || end || waited || sound_first; // Music: the PCM queued is the lead.
        // A scrub preview is a still picture: its epoch never starts the clock (the release lands exactly and does).
        if (!started && !paused && !previewing && have_origin && primed && sink.QueuedFrames() >= (std::max)(4096u, sink.EndpointFrames()) &&
            (music_ || ready_epoch_.load(std::memory_order_acquire) >= epoch || input_done_.load() || waited || sound_first)) {
            if (sink.Start() != EndpointStatus::Ready) { Fail(16, sink.LastError()); break; }
            started = true; started_at = Now();
            if (!clock_started_.load(std::memory_order_relaxed)) {
                preroll_pictures_.store(lead, std::memory_order_relaxed);
                clock_started_.store(started_at, std::memory_order_release);
            }
        }
        // The audio lead: the video packet in hand waits for a surface while the endpoint's PCM runs short: read on for the audio.
        if (started && !paused) {
            if (!lead_from) lead_from = Now() + 10000000;
            const auto least = least_lead_.load(std::memory_order_relaxed), queued_lead = static_cast<std::int64_t>(sink.QueuedFrames()) * 10000000 / out_rate;
            if (Now() >= lead_from && (least < 0 || queued_lead < least)) least_lead_.store(queued_lead, std::memory_order_relaxed);
        }
        if (topping_up && (sink.QueuedFrames() >= lead_high || end || previewing)) topping_up = false;
        if (!reading_ahead && have_packet && video_blocked && held.stream_id == video_.id && audio_.id && gpu_live && !previewing && !end && !stalling &&
            backlog_count + 2 <= kBacklog && !pending() && (topping_up || sink.QueuedFrames() < lead_low)) { // Not when nearly full: no churn.
            topping_up = true;
            backlog_head = (backlog_head + kBacklog - 1) % kBacklog; backlog[backlog_head] = held; ++backlog_count; backlog_parsed = parsed;
            have_packet = parsed = false; reading_ahead = true; read_aheads_.fetch_add(1, std::memory_order_relaxed);
        }
        if (reading_ahead && (end || previewing || backlog_count >= kBacklog || sink.QueuedFrames() >= lead_high)) reading_ahead = false;
        if (!have_packet && backlog_count && !reading_ahead && !preview_shown) {
            held = backlog[backlog_head]; backlog_head = (backlog_head + 1) % kBacklog; --backlog_count;
            have_packet = true; parsed = backlog_parsed; backlog_parsed = false; video_blocked = false;
            if (held.playback_epoch < epoch) held.playback_epoch = epoch; // Read before a continuation: the decode chain's next link.
            if (source_->Poll({held.payload_block_id, held.payload_generation}, block) != IoStatus::Ready) { Fail(18); break; }
        }
        if (!have_packet && !end && !preview_shown) {
            const auto status = parser_.Step(*packets_);
            if (status == demux::ParseStatus::Failed) { Fail(17, static_cast<HRESULT>(parser_.Diagnostic().error)); break; }
            if (status == demux::ParseStatus::Done) end = true;
            if (status != demux::ParseStatus::Backpressure) busy = true; // Moved on, or a read in flight (polled as before).
            // A seek past the last audio packet leaves no media origin: the endpoint's silence clocks the target.
            if (end && !have_origin) { have_origin = true; audio_origin = seek_floor == INT64_MIN ? 0 : seek_floor; begin_timeline(audio_origin); }
            if (const auto* packet = packets_->PeekReadSlot()) {
                held = *packet; have_packet = true; parsed = false; (void)packets_->ReleaseReadSlot();
                if (held.playback_epoch < epoch) {
                    // Read before a forward continuation (only a continuation leaves packets queued across a seek): video
                    // is the decode chain's next link and joins the new epoch; anything else returns unread.
                    if (held.stream_id == video_.id) held.playback_epoch = epoch;
                    else { if (!parser_.ReturnPacket(held)) { Fail(11); break; } have_packet = false; }
                }
                // A preview plays no audio: its packets return unread.
                if (have_packet && previewing && held.stream_id != video_.id) { if (!parser_.ReturnPacket(held)) { Fail(11); break; } have_packet = false; }
                if (have_packet && held.stream_id == video_.id) ++video_taken;
                if (have_packet && reading_ahead && held.stream_id == video_.id) { // Waits behind the one the decoder had no surface for.
                    backlog[(backlog_head + backlog_count) % kBacklog] = held; ++backlog_count; have_packet = false;
                } else if (have_packet && reading_ahead) reading_ahead = false; // This one, then the video again.
                if (have_packet && source_->Poll({held.payload_block_id, held.payload_generation}, block) != IoStatus::Ready) { Fail(18); break; }
            }
        }
        if (have_packet) {
            const std::span<const std::byte> bytes{block.data + held.payload_offset, held.payload_bytes};
            if (held.stream_id == audio_stream) {
                if (skip_decoded_audio) {
                    const auto expected = static_cast<std::int64_t>(audio_units * 10000000 / track_rate);
                    if (held.pts_100ns - audio_base < expected - 1) { // Decoded before the device loss.
                        if (!parser_.ReturnPacket(held)) { Fail(11); break; }
                        have_packet = false;
                    } else skip_decoded_audio = false;
                }
                if (have_packet && !aac->Pending()) {
                    // An AAC priming edit list legitimately starts the audio timeline below zero.
                    if (!audio_packets) {
                        audio_base = held.pts_100ns;
                        // A splice keeps from the frame nearest its target (its timestamps are rounded to 100 ns); a
                        // converted track from the frame at or before it, its converter starting between that frame
                        // and the next, exactly at the target.
                        if (trimming && splicing && converter) {
                            const std::int64_t exact = splice_at > audio_base ? (splice_at - audio_base) * track_rate : 0;
                            trim_frames = exact / 10000000; convert_phase = static_cast<double>(exact % 10000000) / 1e7;
                        } else if (trimming && splicing) trim_frames = splice_at > audio_base ? ((splice_at - audio_base) * track_rate + 5000000) / 10000000 : 0;
                        else if (trimming) trim_frames = seek_floor > audio_base ? (seek_floor - audio_base) * track_rate / 10000000 : 0;
                        else if (!have_origin) { audio_origin = audio_base; have_origin = true; written = 0; probe.Origin(audio_origin); begin_timeline(audio_origin); }
                    }
                    const auto expected = static_cast<std::int64_t>(audio_units * 10000000 / track_rate);
                    const bool opus = aac->Opus();
                    const auto duration = (opus ? 5760ll : 1024ll) * 10000000 / track_rate; // Opus: up to 120 ms a packet.
                    const auto unit = opus ? static_cast<std::uint64_t>((held.duration_100ns * static_cast<std::int64_t>(track_rate) + 5000000) / 10000000) : 1024u;
                    // A shorter access unit is the track's last (any packet after it breaks the expected PTS above): the
                    // encoder's padding past the end of the sound (2026-11-04: ffmpeg's AAC ends 188 x 1024 + 512 for 4 s, and
                    // every such file failed here, its last pictures unshown). Its 1024 frames are played; at most 21 ms of padding.
                    if (held.pts_100ns < audio_base || held.pts_100ns - audio_base < expected - 1 ||
                        held.pts_100ns - audio_base > expected + 1 || held.duration_100ns <= 0 || held.duration_100ns > duration + 1 ||
                        held.pts_100ns > INT64_MAX - held.duration_100ns) { Fail(29); break; }
                    if (!aac->Decode(bytes, held.pts_100ns, held.duration_100ns)) { Fail(19, aac->LastError()); break; }
                    trim();
                    ++audio_packets; audio_units += unit;
                    audio_frames_.store(aac->DecodedFrames()); audio_end_.store(held.pts_100ns + held.duration_100ns);
                    if (!parser_.ReturnPacket(held)) { Fail(11); break; }
                    have_packet = false;
                }
            } else if (held.stream_id == video_.id && !gpu_live) {
                // Device lost: video read meanwhile is returned unread (decoding restarts from the recovery target).
                if (!parser_.ReturnPacket(held)) { Fail(11); break; }
                have_packet = false;
            } else if (held.stream_id == video_.id && av1_) {
                if (held.pts_100ns == core::kUnknownTimestamp) { Fail(29); break; }
                const auto fed = av1_decoder_.Decode(bytes, held.pts_100ns, held.duration_100ns);
                if (fed == Win32VideoMftDecoder::Result::Failed) { Fail(41, av1_decoder_.LastError()); break; }
                if (fed == Win32VideoMftDecoder::Result::Ready) { if (!parser_.ReturnPacket(held)) { Fail(11); break; } have_packet = false; }
                else video_blocked = true; // Full: its pictures first (below), then this unit again.
            } else if (held.stream_id == video_.id) {
                if (!parsed) {
                    if (held.pts_100ns == core::kUnknownTimestamp) { Fail(29); break; }
                    const auto status = hevc_ ? hevc_syntax_.Parse(bytes, video_.nal_length_bytes) : syntax_.Parse(bytes, video_.nal_length_bytes);
                    if (status == codec::SyntaxStatus::Discard || (status == codec::SyntaxStatus::NeedIdr && random_access)) {
                        // RASL picture after a random-access start, or a picture preceding the first random-access
                        // point after a seek: never decoded; its packet returns untouched.
                        if (!parser_.ReturnPacket(held)) { Fail(11); break; }
                        discarded_.fetch_add(1); have_packet = false;
                    } else if (status != codec::SyntaxStatus::Ready) { Fail(20, static_cast<HRESULT>(status)); break; }
                    else { parsed = true; random_access = false; }
                }
                if (have_packet) {
                    DecodePicture native; unsigned width = 0, height = 0; bool output = true, referenced = true;
                    if (hevc_) {
                        const auto* picture = hevc_syntax_.Picture();
                        width = picture->sps->width; height = picture->sps->height; output = picture->output;
                        referenced = picture->retain; // False: a sub-layer non-reference picture of the highest sub-layer.
                    } else {
                        const auto* picture = syntax_.Picture();
                        width = picture->sps->width; height = picture->sps->height;
                        referenced = picture->reference; // nal_ref_idc != 0.
                    }
                    if (width != width_ || height != height_) { Fail(21); break; }
                    // pic_output_flag 0, pictures before the edit-list start and pictures that end before a seek
                    // target are references only (decoded from the sync sample, never presented).
                    // Catching up: shown from where the clock will be once this frame is converted and on screen (a
                    // 150 ms lead covers 4K decode and conversion); the landing frame then waits instead of arriving late.
                    auto shown_from = floor;
                    clock::ClockEstimate estimate{};
                    if (catch_up && clock_.Estimate(Now(), estimate) == clock::ClockQuery::Ready && estimate.epoch == epoch) {
                        shown_from = (std::max)(shown_from, estimate.media_time_100ns + 1500000);
                        if (video_.duration_100ns > 0) shown_from = (std::min)(shown_from, video_.duration_100ns - 1);
                    }
                    // A preview shows the first picture that can be shown (the sync sample itself, in practice).
                    const bool display = output && held.pts_100ns >= 0 &&
                                         (previewing || shown_from == INT64_MIN || held.pts_100ns > shown_from - held.duration_100ns);
                    if (!display && !referenced) {
                        // Neither shown nor predicted from: the picture is not decoded at all. The syntax state moves on
                        // as if it had been (the commit of such a picture never stores its surface), so every later
                        // picture decodes bit for bit as before. On the way from a sync sample to a seek target this
                        // removes the non-reference B-pictures, often most of the pictures in between.
                        SurfaceHandle evicted[16]{}; unsigned count = 0;
                        const bool committed = hevc_ ? hevc_syntax_.Commit({}, evicted, count) : syntax_.Commit({}, evicted, count);
                        if (!committed) { Fail(22); break; }
                        for (unsigned i = 0; i != count; ++i) if (!decoder_.ReleaseReference(evicted[i])) Fail(23);
                        if (!parser_.ReturnPacket(held)) { Fail(11); break; }
                        skipped_.fetch_add(1); have_packet = false; parsed = false;
                        if (work && work->epoch == epoch) ++work->skipped;
                        continue;
                    }
                    native = hevc_ ? hevc_dxva_->Prepare(*hevc_syntax_.Picture(), held, block) : dxva_->Prepare(*syntax_.Picture(), held, block);
                    native.display = display;
                    // Converted with the container's colour description where it has one, the picture's own otherwise,
                    // and nothing left unspecified (the same resolution as Picture().color, for this picture's SPS).
                    native.color_metadata_id = video::PackColor(video::ResolveColor(picture_.container_color,
                        video::UnpackColor(native.color_metadata_id), picture_.bottom - picture_.top));
                    if (native.display) catch_up = false;
                    const bool displayed = native.display;
                    SurfaceHandle surface{};
                    const auto decoded = decoder_.Submit(native, *frames_, *packet_returns_, &surface);
                    if (decoded == DecodeStatus::Submitted) {
                        WakePresenter(); video_blocked = false;
                        SurfaceHandle evicted[16]{}; unsigned count = 0;
                        const bool committed = hevc_ ? hevc_syntax_.Commit(surface, evicted, count) : syntax_.Commit(surface, evicted, count);
                        if (!committed) { Fail(22); break; }
                        for (unsigned i = 0; i != count; ++i) if (!decoder_.ReleaseReference(evicted[i])) Fail(23);
                        have_packet = false; parsed = false;
                        if (displayed && !lead++) lead_since = Now();
                        if (displayed) shown_end = (std::max)(shown_end, held.pts_100ns + held.duration_100ns);
                        if (!displayed && work && work->epoch == epoch) ++work->references;
                        if (displayed && previewing) {
                            // The preview is complete: nothing more is read in this epoch, and its chain is not continued
                            // (the samples after the sync sample were never decoded).
                            preview_shown = true; random_access = true;
                            preview_held_.store(epoch, std::memory_order_release);
                        }
                    } else if (decoded == DecodeStatus::DeviceLost) LoseGpu(2, decoder_.LastError()); // Held until parked.
                    else if (decoded == DecodeStatus::WouldBlock) video_blocked = true; // No surface: the audio lead may read on.
                    else { Fail(24, decoder_.LastError()); break; }
                }
            } else {
                // Undecodable audio (routing_.status), or the track a change has just left: returned untouched.
                if (!parser_.ReturnPacket(held)) { Fail(11); break; }
                have_packet = false;
            }
        }
        if (av1_ && gpu == kGpuRunning) {
            if (end && !have_packet && !backlog_count && !av1_decoder_.Drained() && !av1_decoder_.Drain()) { Fail(41, av1_decoder_.LastError()); break; }
            bool failed = false;
            for (unsigned n = 0; n < 4 && !failed; ++n) {
                // Each 4K upload copies 12 MiB (3-4 ms): a control command waiting (a pause) is read first. Answered after four,
                // a pause stopped the sound up to 31 ms after the picture froze, and at the resume the clock was that far ahead
                // of the next picture (dropped as late, 2026-11-05).
                if (n && !hold_controls_.load(std::memory_order_relaxed) && controls_->PeekReadSlot()) break;
                if (!av1_decoder_.Holding()) {
                    const auto next = av1_decoder_.Next(av1_picture_);
                    if (next == Win32VideoMftDecoder::Result::NeedInput) break;
                    if (next != Win32VideoMftDecoder::Result::Picture) { Fail(41, av1_decoder_.LastError()); failed = true; break; }
                    if (av1_picture_.duration_100ns <= 0) av1_picture_.duration_100ns = 166667;
                    busy = true;
                }
                const auto pts = av1_picture_.pts_100ns, duration = av1_picture_.duration_100ns;
                auto shown_from = floor;
                clock::ClockEstimate estimate{};
                if (catch_up && clock_.Estimate(Now(), estimate) == clock::ClockQuery::Ready && estimate.epoch == epoch) {
                    shown_from = (std::max)(shown_from, estimate.media_time_100ns + 1500000);
                    if (video_.duration_100ns > 0) shown_from = (std::min)(shown_from, video_.duration_100ns - 1);
                }
                if (pts < 0 || (shown_from != INT64_MIN && pts <= shown_from - duration)) { av1_decoder_.Release(); skipped_.fetch_add(1); continue; } // Never shown: no copy.
                av1_picture_.display = true;
                av1_picture_.color_metadata_id = video::PackColor(picture_.color);
                av1_picture_.crop_left = static_cast<std::uint16_t>(picture_.left); av1_picture_.crop_top = static_cast<std::uint16_t>(picture_.top);
                av1_picture_.crop_right = static_cast<std::uint16_t>((std::min)(picture_.right, av1_picture_.width));
                av1_picture_.crop_bottom = static_cast<std::uint16_t>((std::min)(picture_.bottom, av1_picture_.height));
                SurfaceHandle surface{};
                const auto uploaded = decoder_.SubmitUpload(av1_picture_, *frames_, &surface);
                if (uploaded == DecodeStatus::Submitted) {
                    av1_decoder_.Release(); WakePresenter(); video_blocked = false; catch_up = false;
                    if (!lead++) lead_since = Now();
                    shown_end = (std::max)(shown_end, pts + duration);
                } else if (uploaded == DecodeStatus::WouldBlock) { video_blocked = true; break; } // Held until a surface is free.
                else if (uploaded == DecodeStatus::DeviceLost) { LoseGpu(2, decoder_.LastError()); break; }
                else { Fail(24, decoder_.LastError()); failed = true; }
            }
            if (failed) break;
        }
        if (end && !have_packet && !backlog_count && !aac->Pending() && (!audio_.id || aac->Drained()) &&
            (!av1_ || (av1_decoder_.Drained() && !av1_decoder_.Holding())) && !input_done_.load(std::memory_order_relaxed)) {
            input_done_.store(true); WakePresenter(); // The pictures below the reorder depth may go now.
        }
        sink_signaled = sink_signaled && started && !paused; // Consumed only by the Pump above.
        idle(busy || gpu != kGpuRunning || committed_frames != pass_frames || audio_packets != pass_audio || epoch != pass_epoch ||
             have_packet != pass_packet || started != pass_started || paused != pass_paused || end != pass_end, now);
    }
    if (have_packet) (void)parser_.ReturnPacket(held);
    return_backlog(false);
    if (started && !paused) (void)sink.Stop();
    audio_recovery_ = sink.Recovery(); audio_attached_ = sink.Attached();
    WorkerPhase(Phase::WorkerStopped);
    stopped_.store(true);
    // Native destruction happens only after the caller ends allocation measurement.
    while (!teardown_.load()) YieldProcessor();
    WorkerPhase(Phase::WorkerTeardown);
    for (auto& decoder : decoders) decoder.Close();
    av1_decoder_.Close();
    if (warm_ && warm_->sink_ready) {
        // Kept for the next file: stopped, its queued PCM dropped, its position and statistics started again.
        if (sink.Reset() != EndpointStatus::Ready) { sink.Close(); warm_->sink_ready = false; }
        sink.SetOutputGain(nullptr, nullptr); // This coordinator's gain is about to go.
    } else sink.Close();
    if (SUCCEEDED(mf)) (void)MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
}
bool PlaybackCoordinator::Run() noexcept {
    if (!ready_.load() || !arena_.IsSealed()) return false;
    // Software decode (AV1 through the platform's MFT): 4K60 keeps 8 decoder threads busy at normal priority, and the presentation
    // thread, paced and mostly waiting, woke up to 17 ms after a deadline behind them (a picture dropped in 7 runs of 10, "waited for
    // nothing"). It goes before them while this file plays; the caller's priority comes back on return.
    struct Priority {
        HANDLE thread = GetCurrentThread(); int before = GetThreadPriority(GetCurrentThread()); bool raised = false;
        ~Priority() { if (raised) (void)SetThreadPriority(thread, before); }
    } priority;
    if (av1_ && priority.before < THREAD_PRIORITY_HIGHEST) priority.raised = SetThreadPriority(priority.thread, THREAD_PRIORITY_HIGHEST) != FALSE;
    // Smart resume: the seek is queued before the worker starts, so the first picture decoded is the landing.
    if (resume_.known && options_.resume) {
        const auto at = session::ResumeLedger::ResumeAt(resume_.entry);
        if (at > 0 && seekable_ && SendSeek(at)) { resume_.resumed = true; resume_.target = seek_target_; resume_.epoch = epoch_; }
    }
    renderer_.ResetInputStatistics(); // Input waits measured from the run's start (not the window's creation).
    const auto begin = Now(); run_began_ = begin; start_.store(true);
    { HostEvent opened{}; opened.kind = HostEvent::kOpened; opened.time = begin; opened.duration = duration_; opened.width = display_width_; opened.height = display_height_; Emit(opened); }
    KeepAwake(!paused_);
    if (bookmarks_.Count()) BookmarksChanged(); // The file's bookmarks, on the dock from the start.
    if (routing_.status == AudioStatus::UnsupportedLayout) {
        char cue[ui::StatusCue::kMaxBytes];
        std::snprintf(cue, sizeof(cue), "Audio \xc2\xb7 %u-channel AAC cannot play here \xc2\xb7 picture only", routing_.source_channels);
        Status(cue, ui::StatusIcon::Warning, ui::StatusTone::Warning);
    }
    StartCue();
    unsigned endpoint_moves = 0;
    bool script_paused = false, script_done = !options_.pause_ms;
    unsigned hud_script = 0;
    unsigned dsp_script = 0, surround_script = 0, neural_script = 0, volume_script = 0, quality_script = 0, reuse_script = 0, paused_script = 0;
    render::NeuralUpscalePass::TileCounts reuse_tiles{}; render::D3D12Renderer::GpuTotal reuse_gpu{}; unsigned reuse_presented = 0, reuse_dropped = 0;
    std::int64_t reuse_landed = 0; unsigned reuse_marks = 4, reuse_marked[4]{};
    std::uint32_t reuse_epoch = 0; // The phase's seek, whose landing starts its count (0: counting).
    const unsigned command_count = options_.commands ? (std::min)(options_.command_count, kMaxCommands) : 0;
    // Every scripted command given, and each seek among them on screen (or overtaken by a later seek).
    const unsigned typing_count = options_.typing ? options_.typing_count : 0;
    unsigned typed = 0;
    const auto commands_done = [&] {
        if (command_results_ < command_count || typed != typing_count) return false;
        for (unsigned i = 0; i < command_results_; ++i) if (commands_[i].epoch == epoch_ && !commands_[i].landed) return false;
        return true;
    };
    // Scrub script state.
    unsigned scrub_phase = 0, burst_before = 0; std::uint32_t seen_epoch = 0, burst_epoch = 0;
    std::int64_t drag_begin = 0, last_event = 0, last_request = 0, last_landing = 0, phase_time = 0;
    for (;;) {
        const auto pumping = Now();
        ui_beats_.store(ui_beats_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); UiPhase(Phase::Messages);
        if (error_.load() || !renderer_.Messages()) break;
        UiPhase(Phase::Scripts);
        if (file_request_ != FileRequest::None) break; // The caller opens the file asked for.
        PumpSteps();
        PumpStats();
        if (options_.test_zoom_live && zoom_script_ < 4) {
            // Live zoom while playing (PlaybackOptions::test_zoom_live): each change once the previous one is on screen natively.
            const auto since = Now() - run_began_;
            const bool settled = !renderer_.LiveZooms() || renderer_.LiveZoomAt(renderer_.LiveZooms() - 1).native != 0;
            static constexpr std::int64_t kAt[]{15000000, 30000000, 45000000, 60000000};
            if (since >= kAt[zoom_script_] && settled && !paused_ && clock_started_.load(std::memory_order_acquire)) {
                if (zoom_script_ == 0) (void)Command("zoom in");
                else if (zoom_script_ == 1) {
                    const auto v = renderer_.LastVideoRect();
                    POINT p{static_cast<LONG>(v.x + v.width / 4), static_cast<LONG>(v.y + v.height / 3)}; ClientToScreen(renderer_.Window(), &p);
                    renderer_.Deliver(WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 2 * WHEEL_DELTA), MAKELPARAM(p.x, p.y));
                } else if (zoom_script_ == 2) renderer_.Deliver(WM_KEYDOWN, VK_RIGHT, 0);
                else (void)Command("reset zoom");
                ++zoom_script_;
            }
        }
        if (options_.test_ab_loop && !loop_run_.done) {
            // --test-ab-loop: L at 1.0 s and at 2.0 s of media, three turns, L (off); then the sentence on screen, three turns, off.
            auto& step = loop_script_;
            const auto position = PrecisePosition();
            const bool playing = !paused_ && renderer_.LastSeek().epoch == epoch_ && clock_started_.load(std::memory_order_acquire) != 0 && anchored_at_.load(std::memory_order_acquire) != 0;
            if (step == 0 && playing && position >= 10000000) { renderer_.Deliver(WM_KEYDOWN, 'L', 0); ++step; }
            else if (step == 1 && playing && position >= 20000000) { renderer_.Deliver(WM_KEYDOWN, 'L', 0); ++step; }
            else if (step == 2 && loop_run_.count >= 3 && !loop_epoch_ && playing && position >= 15000000) { renderer_.Deliver(WM_KEYDOWN, 'L', 0); loop_mark_ = Now(); ++step; }
            else if (step == 3 && playing && Now() - loop_mark_ >= 5000000) {
                const auto r = Command("loop this sentence");
                if (r.status == ai::IntentStatus::Ok) ++step; else { loop_run_.done = true; step = 99; }
            } else if (step == 4 && loop_run_.count >= 6 && !loop_epoch_ && playing) {
                (void)Command("\xd7\x91\xd7\x98\xd7\x9c \xd7\x9c\xd7\x95\xd7\xa4"); // "בטל לופ"
                loop_run_.done = true; ++step;
            }
        }
        if (options_.test_bookmarks && !bookmark_run_.done) {
            // --test-bookmarks (PlaybackOptions::test_bookmarks): each step once the one before has landed.
            auto& step = bookmark_script_; auto& r = bookmark_run_;
            const auto position = PrecisePosition(); const auto at = Now();
            const bool playing = !paused_ && renderer_.LastSeek().epoch == epoch_ && clock_started_.load(std::memory_order_acquire) != 0;
            const auto jump = [&](const char* command, unsigned number) {
                if (r.jump_count >= std::size(r.jumps)) return;
                auto& j = r.jumps[r.jump_count];
                if (Command(command).status == ai::IntentStatus::Ok) { j = {}; j.number = number; j.target = bookmarks_.At(number - 1); j.epoch = epoch_; ++r.jump_count; ++step; }
                else { r.done = true; step = 99; }
            };
            const bool settled = !r.jump_count || r.jumps[r.jump_count - 1].landed;
            if (r.jump_count && !r.jumps[r.jump_count - 1].landed && renderer_.LastSeek().epoch == r.jumps[r.jump_count - 1].epoch) {
                auto& j = r.jumps[r.jump_count - 1]; const auto& l = renderer_.LastSeek();
                j.landed = true; j.pts = l.pts; j.duration = l.duration; j.latency = l.latency; bookmark_mark_ = at;
            }
            if (step == 0 && playing && position >= 10000000) { renderer_.Deliver(WM_KEYDOWN, 'B', 0); r.added[0] = bookmarks_.At(0); ++step; }
            else if (step == 1 && playing && position >= 25000000) { (void)Command("add bookmark"); r.added[1] = bookmarks_.At(1); ++step; }
            else if (step == 2 && playing && position >= 40000000) { (void)Command("\xd7\xa1\xd7\x9e\xd7\x9f \xd7\x94\xd7\xa2\xd7\xa8\xd7\x94"); r.added[2] = bookmarks_.At(2); ++step; } // "סמן הערה"
            else if (step == 3 && playing) jump("go to bookmark 1", 1);
            else if (step == 4 && settled && at - bookmark_mark_ >= 5000000) jump("next bookmark", 2);
            else if (step == 5 && settled && at - bookmark_mark_ >= 5000000) jump("\xd7\x9c\xd7\x9a \xd7\x9c\xd7\xa1\xd7\x99\xd7\x9e\xd7\xa0\xd7\x99\xd7\x99\xd7\x94 3", 3); // "לך לסימנייה 3"
            else if (step == 6 && settled && at - bookmark_mark_ >= 5000000) jump("previous bookmark", 2);
            else if (step == 7 && settled && at - bookmark_mark_ >= 3000000) { if (RequestPause(true)) { bookmark_mark_ = at; ++step; } }
            else if (step == 8 && at - bookmark_mark_ >= 3000000) {
                // The paused picture (bookmark 2's) as the reference: "הקפא פריים להשוואה".
                if (Command("\xd7\x94\xd7\xa7\xd7\xa4\xd7\x90 \xd7\xa4\xd7\xa8\xd7\x99\xd7\x99\xd7\x9d \xd7\x9c\xd7\x94\xd7\xa9\xd7\x95\xd7\x95\xd7\x90\xd7\x94").status == ai::IntentStatus::Ok) {
                    r.pinned = renderer_.ReferencePts(); bookmark_mark_ = at; ++step;
                } else { r.done = true; step = 99; }
            }
            else if (step == 9 && at - bookmark_mark_ >= 2000000) jump("go to bookmark 3", 3); // Paused: lands and holds there.
            else if (step == 10 && settled && at - bookmark_mark_ >= 3000000) {
                renderer_.Deliver(WM_KEYDOWN, 'V', 0); renderer_.Deliver(WM_KEYDOWN, 'V', 0); // Enhanced, then Pinned | Live.
                bookmark_mark_ = at; ++step;
            }
            else if (step == 11 && at - bookmark_mark_ >= 10000000) { r.live = renderer_.ShownPts(); renderer_.RequestCapture(); ++step; } // Re-conversions done.
            else if (step == 12 && renderer_.Captures() >= 1) { r.held = renderer_.CaptureAt(0); renderer_.RequestReferenceCapture(); ++step; }
            else if (step == 13 && renderer_.Captures() >= 2) { r.reference = renderer_.CaptureAt(1); renderer_.RequestScreenCapture(); ++step; }
            else if (step == 14 && renderer_.Captures() >= 3) {
                r.screen = renderer_.CaptureAt(2);
                renderer_.Deliver(WM_KEYDOWN, 'V', 0); // Off.
                if (RequestPause(false)) { bookmark_mark_ = at; r.done = true; ++step; }
            }
        }
        if (options_.test_telemetry && !telemetry_run_.done) {
            // --test-telemetry: the panel, an overload that makes the governor step down, then the explanations.
            auto& step = telemetry_script_; auto& t = telemetry_run_;
            const auto since = Now() - run_began_;
            const bool playing = !paused_ && clock_started_.load(std::memory_order_acquire) != 0;
            const unsigned changes = renderer_.Governor().Changes();
            if (step == 0 && playing && since >= 10000000) {
                renderer_.Deliver(WM_KEYDOWN, 'K', 0); // Night mode on: the panel shows its gain.
                renderer_.Deliver(WM_KEYDOWN, 'I', 0); ++step;
            }
            else if (step == 1 && since >= 20000000) { renderer_.SetTestLoad(40.0f); t.load_on = Now(); telemetry_mark_ = changes; ++step; }
            else if (step == 2 && changes > static_cast<unsigned>(telemetry_mark_) && renderer_.Governor().LastChange().reason == render::QualityGovernor::Reason::Overload) {
                renderer_.SetTestLoad(0); (void)Command("why did quality change?"); telemetry_mark_ = Now(); ++step;
            } else if (step == 2 && since >= 120000000) { t.done = true; step = 99; } // No step down within 12 s: recorded as such.
            else if (step == 3 && Now() - telemetry_mark_ >= 10000000) { (void)Command("\xd7\x94\xd7\xa6\xd7\x92 \xd7\xa0\xd7\xaa\xd7\x95\xd7\xa0\xd7\x99\xd7\x9d"); telemetry_mark_ = Now(); ++step; } // "הצג נתונים"
            else if (step == 4 && Now() - telemetry_mark_ >= 10000000) { (void)Command("\xd7\x9c\xd7\x9e\xd7\x94 \xd7\x94\xd7\x90\xd7\x99\xd7\x9b\xd7\x95\xd7\xaa \xd7\x99\xd7\xa8\xd7\x93\xd7\x94?"); telemetry_mark_ = Now(); ++step; } // "למה האיכות ירדה?"
            else if (step == 5 && Now() - telemetry_mark_ >= 10000000) { (void)Command("status report"); telemetry_mark_ = Now(); ++step; }
            else if (step == 6 && Now() - telemetry_mark_ >= 10000000) { renderer_.Deliver(WM_KEYDOWN, 'I', 0); t.done = true; ++step; }
        }
        if (options_.test_zoom && !zoom_run_.done) {
            // Event-driven: each change waits for the capture before it, each capture for the re-converted picture.
            auto& z = zoom_run_; auto& step = zoom_script_;
            const auto since = Now() - run_began_;
            const auto record = [&] {
                auto& r = z.steps[z.count < 5 ? z.count : 4];
                r.zoom = renderer_.Zoom(); r.x = renderer_.ZoomX(); r.y = renderer_.ZoomY(); r.region = renderer_.LastRegion();
                r.latency = renderer_.PausedUpdates() ? renderer_.PausedUpdateAt(renderer_.PausedUpdates() - 1).latency : 0;
                renderer_.RequestCapture();
            };
            if (step == 0) { if (since >= 10000000 && RequestPause(true)) ++step; }
            else if (step == 1) { if (since >= 15000000) { zoom_updates_ = renderer_.PausedUpdates(); record(); ++step; } }
            else if (step % 2 == 0 && step < 10) {
                if (renderer_.Captures() >= step / 2) {
                    const auto& h = z.steps[step / 2 - 1]; (void)h;
                    z.steps[step / 2 - 1].left = renderer_.CaptureAt(step / 2 - 1).left; z.steps[step / 2 - 1].right = renderer_.CaptureAt(step / 2 - 1).right;
                    if (step == 2) (void)Command("zoom in");
                    else if (step == 4) {
                        // Three notches with Ctrl, at three quarters across and half way down the picture, in screen coordinates.
                        const auto v = renderer_.LastVideoRect();
                        POINT p{static_cast<LONG>(v.x + v.width * 3 / 4), static_cast<LONG>(v.y + v.height / 2)}; ClientToScreen(renderer_.Window(), &p);
                        renderer_.Deliver(WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 3 * WHEEL_DELTA), MAKELPARAM(p.x, p.y));
                    } else if (step == 6) renderer_.Deliver(WM_KEYDOWN, VK_LEFT, 0);
                    else if (step == 8) (void)Command("\xd7\x90\xd7\xa4\xd7\xa1 \xd7\x96\xd7\x95\xd7\x9d");
                    ++step;
                }
            } else if (step % 2 == 1 && step < 10) {
                if (renderer_.PausedUpdates() > zoom_updates_ + (step - 3) / 2) { ++z.count; record(); ++step; }
            } else if (step == 10 && renderer_.Captures() >= 5) {
                z.steps[4].left = renderer_.CaptureAt(4).left; z.steps[4].right = renderer_.CaptureAt(4).right; ++z.count;
                if (RequestPause(false)) { z.done = true; ++step; }
            }
        }
        if (options_.test_window_stall && stall_script_ < 3) {
            // --test-window-stall: the window's procedure blocks 120 ms at 1, 2 and 3 s of playing; a tagged mouse move every
            // 250 ms (its handling measured from the window thread's queueing: InputStatistics).
            const bool playing = !paused_ && renderer_.LastSeek().epoch == epoch_ && clock_started_.load(std::memory_order_acquire) != 0;
            const auto since = Now() - run_began_;
            if (playing && since >= 10000000 + static_cast<std::int64_t>(stall_script_) * 10000000 && renderer_.StallWindow(120)) { ++stall_script_; stall_mark_ = Now(); }
        } else if (options_.test_window_stall && Now() - stall_mark_ >= 10000000) (void)renderer_.Deliver(WM_KEYDOWN, VK_ESCAPE, 0); // A second after the last: done.
        if (options_.test_window_stall && Now() - stall_input_mark_ >= 2500000 && clock_started_.load(std::memory_order_acquire) != 0) {
            const auto v = renderer_.LastVideoRect();
            if (renderer_.PostTestInput(WM_MOUSEMOVE, 0, MAKELPARAM(v.x + v.width / 3, v.y + v.height / 3))) stall_input_mark_ = Now();
        }
        if (options_.test_rotation && !rotation_run_.done) {
            // --test-rotation: event-driven; each picture captured once the change before it is on screen.
            auto& r = rotation_run_; auto& step = rotation_script_;
            const bool playing = !paused_ && renderer_.LastSeek().epoch == epoch_ && clock_started_.load(std::memory_order_acquire) != 0;
            const auto take = [&](bool held) { // The paused picture (held), or the picture as composed while playing.
                auto& s = r.steps[r.count < 5 ? r.count : 4];
                s.orientation = renderer_.OrientationNow(); s.paused = paused_; rotation_captures_ = renderer_.Captures();
                if (held) renderer_.RequestCapture(); else renderer_.RequestScreenCapture();
            };
            const auto store = [&] {
                if (renderer_.Captures() <= rotation_captures_ || r.count >= 5) return false;
                const auto& c = renderer_.CaptureAt(renderer_.Captures() - 1); auto& s = r.steps[r.count];
                s.width = c.width; s.height = c.height; for (unsigned k = 0; k < 16; ++k) s.grid[k] = c.grid[k];
                ++r.count; return true;
            };
            const auto updated = [&] { return renderer_.PausedUpdates() > rotation_updates_; };
            if (step == 0) { if (playing && Now() - run_began_ >= 10000000) { take(false); ++step; } }
            else if (step == 1) { if (store()) { renderer_.Deliver(WM_KEYDOWN, 'R', 0); rotation_mark_ = Now(); ++step; } } // R, playing.
            else if (step == 2) { if (Now() - rotation_mark_ >= 5000000) { take(false); ++step; } }
            else if (step == 3) { if (store() && RequestPause(true)) { rotation_mark_ = Now(); ++step; } }
            else if (step == 4) {
                if (paused_ && Now() - rotation_mark_ >= 3000000) { rotation_updates_ = renderer_.PausedUpdates(); renderer_.Deliver(WM_KEYDOWN, 'R', 0); ++step; } // R, paused.
            } else if (step == 5) { if (updated()) { take(true); ++step; } }
            else if (step == 6) {
                if (store()) { rotation_updates_ = renderer_.PausedUpdates(); (void)Command("level horizon right"); rotation_levels_ = 1; ++step; }
            } else if (step == 7) {
                if (updated()) {
                    rotation_updates_ = renderer_.PausedUpdates();
                    if (rotation_levels_ < 3) { (void)Command("level horizon right"); ++rotation_levels_; } else { take(true); ++step; }
                }
            } else if (step == 8) {
                if (store()) { rotation_updates_ = renderer_.PausedUpdates(); (void)Command("straighten"); ++step; }
            } else if (step == 9) {
                if (updated()) { rotation_updates_ = renderer_.PausedUpdates(); (void)Command("\xd7\xa1\xd7\x95\xd7\x91\xd7\x91 \xd7\xaa\xd7\x9e\xd7\x95\xd7\xa0\xd7\x94"); ++step; } // סובב תמונה
            } else if (step == 10) { if (updated()) { take(true); ++step; } }
            else if (step == 11) { if (store() && RequestPause(false)) { r.done = true; ++step; } }
        }
        if (options_.test_auto_horizon && !auto_horizon_run_.done) {
            // --test-auto-horizon: on the media position; each picture captured once, as composed while playing.
            auto& r = auto_horizon_run_; auto& step = auto_horizon_script_;
            const bool playing = !paused_ && renderer_.LastSeek().epoch == epoch_ && clock_started_.load(std::memory_order_acquire) != 0;
            const auto position = renderer_.MediaPosition();
            const auto take = [&] {
                auto& s = r.steps[r.count < 3 ? r.count : 2];
                s.position = position; s.level = renderer_.LevelDegrees(); s.tilt = renderer_.Horizon().Tilt(); s.locked = renderer_.Horizon().Locked();
                s.on = renderer_.AutoLevel(); s.orientation = renderer_.OrientationNow();
                s.cropped = renderer_.AutoCropped(); s.bars = renderer_.Bars().Bars(); s.active = renderer_.Bars().Active(); s.region = renderer_.ViewRegion();
                auto_horizon_captures_ = renderer_.Captures(); renderer_.RequestScreenCapture();
            };
            const auto store = [&] {
                if (renderer_.Captures() <= auto_horizon_captures_ || r.count >= 3) return false;
                const auto& c = renderer_.CaptureAt(renderer_.Captures() - 1); auto& s = r.steps[r.count];
                s.width = c.width; s.height = c.height; s.shown = c.horizon; for (unsigned k = 0; k < 16; ++k) s.grid[k] = c.grid[k];
                s.extents = c.extents; s.extents_found = c.extents_found;
                ++r.count; return true;
            };
            if (step == 0) { if (playing && position >= 5000000) { renderer_.SetQualityMode(render::QualityMode::Fast); take(); ++step; } }
            else if (step == 1) {
                if (store()) { // Shift+R, with this thread's key state as the keyboard driver leaves it while Shift is held.
                    BYTE keys[256]{}; (void)GetKeyboardState(keys);
                    const BYTE shift = keys[VK_SHIFT]; keys[VK_SHIFT] = 0x80; (void)SetKeyboardState(keys);
                    renderer_.Deliver(WM_KEYDOWN, 'R', 0);
                    keys[VK_SHIFT] = shift; (void)SetKeyboardState(keys);
                    ++step;
                }
            } else if (step == 2) { if (playing && position >= 28000000) { take(); ++step; } }
            else if (step == 3) { if (store()) { if (options_.test_crop_bars) (void)Command("recadrer les bandes noires"); ++step; } }
            else if (step == 4) { if (playing && position >= 58000000) { take(); ++step; } }
            else if (step == 5) {
                if (store()) { r.measured = renderer_.Horizon().Measured(); r.readbacks = renderer_.HorizonReadbacks(); r.done = true; ++step; }
            }
        }
        if (options_.test_music && !music_run_.done) {
            // --test-music: event-driven on the media position (PlaybackOptions::test_music).
            auto& m = music_run_; auto& step = music_script_;
            const auto position = renderer_.MediaPosition(); const auto at = Now();
            const bool playing = !paused_ && renderer_.LastSeek().epoch == epoch_ && clock_started_.load(std::memory_order_acquire) != 0;
            const auto loudest = [&] {
                const float* b = renderer_.ShownBands(); unsigned best = 0;
                for (unsigned k = 1; k < audio::AudioPicture::kBands; ++k) if (b[k] > b[best]) best = k;
                return best;
            };
            if (step == 0 && playing && position >= 10000000) {
                m.rate_from = at; m.rate_presents = renderer_.MusicPresents();
                m.expected[0] = analyser_.BandOf(440); m.expected[1] = analyser_.BandOf(1760); m.expected[2] = analyser_.BandOf(880);
                ++step;
            } else if (step == 1 && playing && position >= 20000000) {
                m.rate = static_cast<double>(renderer_.MusicPresents() - m.rate_presents) * 1e7 / static_cast<double>(at - m.rate_from);
                m.bands[0] = loudest();
                if (Command("next chapter").status == ai::IntentStatus::Ok) { music_epoch_ = epoch_; ++step; }
            } else if (step == 2 && playing && renderer_.LastSeek().epoch == music_epoch_ && position >= renderer_.LastSeek().pts + 6000000) {
                m.landing = renderer_.LastSeek().pts; m.landing_latency = renderer_.LastSeek().latency; m.bands[1] = loudest();
                if (RequestPause(true)) { m.paused_pictures = renderer_.AudioPictures(); m.paused_presents = renderer_.MusicPresents(); music_mark_ = at; ++step; }
            } else if (step == 3 && at - music_mark_ >= 5000000) {
                m.paused_pictures = renderer_.AudioPictures() - m.paused_pictures; m.paused_presents = renderer_.MusicPresents() - m.paused_presents;
                if (RequestPause(false)) ++step;
            } else if (step == 4 && playing && position >= 65000000) {
                if (Command("speed 1.5x").status == ai::IntentStatus::Ok) ++step;
            } else if (step == 5 && playing && position >= 80000000) { m.bands[2] = loudest(); m.done = true; ++step; }
        }
        if (options_.test_stabilize && !renderer_.ScreenCaptureWaiting()) renderer_.RequestScreenCapture();
        if (options_.test_stabilize && !steady_run_.done) {
            // --test-stabilize (docs/picture.md, "Stabilising the window"): the shaken clip as it comes, then "stabiliser la vidéo", then
            // Alt+S: the screen's spot through it all, a capture at a time.
            const auto at = Now() - run_began_;
            auto& r = steady_run_;
            if (steady_script_ == 0 && at >= 25000000) { r.on_at = renderer_.ShownPts(); r.status_on = Command("stabiliser la vid\xc3\xa9o").status; ++steady_script_; }
            else if (steady_script_ == 1 && at >= 62000000) { r.off_at = renderer_.ShownPts(); r.pictures = renderer_.SteadiedPictures(); r.measured = renderer_.SteadyMeasured(); renderer_.Deliver(WM_SYSKEYDOWN, 'S', LPARAM{1} << 29); ++steady_script_; }
            else if (steady_script_ == 2 && at >= 64000000) { r.off_shown = !renderer_.Stabilization(); r.status_again = Command("stabilize off").status; r.done = true; ++steady_script_; }
        }
        if (options_.test_speed && !speed_run_.done) {
            // The clock's speed phases, from the clock itself.
            clock::ClockEstimate e{};
            const auto at = Now();
            if (clock_.Estimate(at, e) == clock::ClockQuery::Ready && !paused_) {
                auto& r = speed_run_;
                if (!r.phase_count || r.phases[r.phase_count - 1].speed != static_cast<float>(e.speed)) {
                    if (r.phase_count < std::size(r.phases)) r.phases[r.phase_count++] = {static_cast<float>(e.speed), at, e.media_time_100ns, at, e.media_time_100ns};
                } else { auto& p = r.phases[r.phase_count - 1]; p.to_now = at; p.to_media = e.media_time_100ns; }
            }
            static constexpr std::int64_t kAt[]{15000000, 30000000, 45000000, 65000000, 80000000};
            const auto since = at - run_began_;
            auto& step = speed_script_;
            if (step < 4 && since >= kAt[step]) {
                if (step == 0) renderer_.Deliver(WM_CHAR, L']', 0);
                else (void)Command(step == 1 ? "speed 2x" : step == 2 ? "half speed" : "\xd7\x9e\xd7\x94\xd7\x99\xd7\xa8\xd7\x95\xd7\xaa \xd7\xa8\xd7\x92\xd7\x99\xd7\x9c\xd7\x94");
                ++step;
            } else if (step >= 4 && step < 9 && since >= kAt[4] && !pending_steps_ && !step_epoch_ && (step == 4 || frame_steps_ >= step - 4) &&
                       // The first step cuts a mark short (the PCM committed so far ends inside one), or goes after a second without one.
                       (step != 4 || since >= kAt[4] + 10000000 || speed_in_mark_.load(std::memory_order_relaxed))) {
                if (step > 4) speed_run_.steps[speed_run_.step_count++] = renderer_.ShownPts(); else speed_run_.start_pts = renderer_.ShownPts();
                renderer_.Deliver(WM_CHAR, step < 7 ? L'.' : L',', 0);
                ++step;
            } else if (step == 9 && !pending_steps_ && !step_epoch_ && frame_steps_ >= 5) {
                speed_run_.steps[speed_run_.step_count++] = renderer_.ShownPts(); speed_run_.done = true;
                renderer_.Deliver(WM_KEYDOWN, VK_ESCAPE, 0);
            }
        }
        const auto elapsed = Now() - begin;
        if (clock_started_.load(std::memory_order_relaxed)) { stalls_.messages_sum += elapsed - (pumping - begin); ++stalls_.passes; }
        if (elapsed - (pumping - begin) > stalls_.messages && clock_started_.load(std::memory_order_relaxed)) { stalls_.messages = elapsed - (pumping - begin); stalls_.messages_pts = renderer_.ShownPts(); }
        if (options_.test_volume && volume_script < 5 && elapsed >= 10000000 + static_cast<std::int64_t>(volume_script) * 3500000) {
            // Real key messages through the demo's handler: Down x4 (100 % -> 80 %), M (mute), M (unmute), Up x2 (90 %),
            // then Down x18 (the floor: 0 %, silence) and Up x18 again (90 %).
            const auto press = [&](WPARAM key, unsigned times) { for (unsigned i = 0; i < times; ++i) renderer_.Deliver(WM_KEYDOWN, key, 0); };
            if (volume_script == 0) press(VK_DOWN, 4); else if (volume_script == 1 || volume_script == 2) press('M', 1);
            else if (volume_script == 3) press(VK_UP, 2); else { press(VK_DOWN, 18); press(VK_UP, 18); }
            ++volume_script;
        }
        // --test-chapters: each step waits for what it depends on instead of a time (a stall on a loaded machine shifted
        // the timeline once and a step found no scene): the previous seek landed and its clock running, the position
        // reached, and for the scene steps the live cut seen. Page Up comes between the sixth and seventh command.
        bool chapter_step = false;
        if (options_.test_chapters) {
            constexpr std::int64_t kS = 10000000;
            const auto& landed = renderer_.LastSeek();
            const bool settled = landed.epoch == epoch_ && anchored_at_.load(std::memory_order_acquire) > landed.presented;
            const auto position = renderer_.MediaPosition();
            const auto seen = [&](std::int64_t at) {
                for (unsigned c = 0; c < timeline_.Scenes(); ++c)
                    if (timeline_.SceneAt(c).origin == timeline::SceneOrigin::Live && std::llabs(timeline_.SceneAt(c).at - at) < kS / 2) return true;
                return false;
            };
            static constexpr std::int64_t kFrom[]{10, 40, 70, 40, 45, 40, 40}; // Tenths of a second, per command.
            if (command_results_ < command_count && command_results_ < std::size(kFrom) && settled && position >= kFrom[command_results_] * kS / 10 &&
                (command_results_ != 4 || seen(3 * kS)) && (command_results_ != 6 || chapter_script_))
                chapter_step = true;
            if (command_results_ == 6 && !chapter_script_ && settled && seen(6 * kS) && position >= 74 * kS / 10) {
                chapter_script_ = true; renderer_.Deliver(WM_KEYDOWN, VK_PRIOR, 0); // Page Up: the chapter key.
            }
        }
        if (options_.test_isolation && !isolation_posted_ && elapsed >= 15000000) {
            // Posted through the queue like the desktop's input, but tagged: the isolation admits these and nothing else.
            isolation_posted_ = true;
            for (unsigned k = 0; k < 4; ++k) (void)renderer_.PostTestInput(WM_KEYDOWN, VK_DOWN, 1);
        }
        if (command_results_ < command_count && (options_.test_chapters ? chapter_step : elapsed >= static_cast<std::int64_t>(options_.commands[command_results_].at_ms) * 10000)) {
            auto& r = commands_[command_results_]; r = {};
            const char* text = options_.commands[command_results_].text;
            const auto asked = Now();
            r.intent = Command(text ? std::string_view{text} : std::string_view{});
            r.resolve = Now() - asked; r.volume = Volume(); r.muted = Muted(); r.paused = paused_;
            r.audio_track = audio_choice_; r.subtitle_track = subtitle_choice_;
            r.by_concept = r.intent.kind == ai::IntentKind::FindSubtitle && subtitle_seek_.by_concept;
            { const auto shown = renderer_.Status().Text(); std::memcpy(r.status, shown.data(), (std::min)(shown.size(), sizeof(r.status) - 1)); }
            const bool seek = SeekingIntent(r.intent.kind);
            if (seek && r.intent.status == ai::IntentStatus::Ok) r.epoch = epoch_;
            ++command_results_;
        }
        if (typed < typing_count && elapsed >= static_cast<std::int64_t>(options_.typing[typed].at_ms) * 10000) {
            // Real window messages, as TranslateMessage and the keyboard driver would produce them.
            for (const wchar_t* key = options_.typing[typed].keys; key && *key; ++key) {
                if (*key == L'\r') renderer_.Deliver(WM_KEYDOWN, VK_RETURN, 0);
                else if (*key == L'\x1b') renderer_.Deliver(WM_KEYDOWN, VK_ESCAPE, 0);
                else if (*key == L'\b') renderer_.Deliver(WM_KEYDOWN, VK_BACK, 0);
                else if (*key == L'\x1c' || *key == L'\x1d') {
                    // Ctrl+Left / Ctrl+Right: the thread's key state as the keyboard driver leaves it while Control is held.
                    BYTE keys[256]{}; (void)GetKeyboardState(keys);
                    const BYTE control = keys[VK_CONTROL]; keys[VK_CONTROL] = 0x80; (void)SetKeyboardState(keys);
                    renderer_.Deliver(WM_KEYDOWN, *key == L'\x1d' ? VK_RIGHT : VK_LEFT, 0);
                    keys[VK_CONTROL] = control; (void)SetKeyboardState(keys);
                }
                else renderer_.Deliver(WM_CHAR, *key, 0);
            }
            ++typed;
        }
        if (resume_.resumed && !resume_.landed && renderer_.LastSeek().epoch == resume_.epoch) {
            const auto& landed = renderer_.LastSeek();
            resume_.landed = true; resume_.pts = landed.pts; resume_.duration = landed.duration; resume_.latency = landed.latency;
        }
        for (unsigned i = 0; i < command_results_; ++i) {
            auto& r = commands_[i]; const auto& landed = renderer_.LastSeek();
            if (r.epoch && !r.landed && landed.epoch == r.epoch) {
                r.landed = true; r.pts = landed.pts; r.duration = landed.duration; r.latency = landed.latency; r.subtitles = landed.subtitles;
            }
        }
        if (options_.test_dsp && dsp_script < 2 && elapsed >= (dsp_script == 0 ? 4000000 : 9000000)) {
            // Real window messages: strong -> off -> light, spatial on -> off -> on.
            // Held-key repeats must not cycle/toggle a second time.
            for (const WPARAM key : {WPARAM{'D'}, WPARAM{'S'}}) {
                // Spatial audio is Shift+S (2026-11-01: S takes a snapshot): the thread's key state as the driver leaves it with Shift held.
                BYTE keys[256]{}; (void)GetKeyboardState(keys);
                const BYTE shift = keys[VK_SHIFT];
                if (key == 'S') { keys[VK_SHIFT] = 0x80; (void)SetKeyboardState(keys); }
                renderer_.Deliver(WM_KEYDOWN, key, 0);
                renderer_.Deliver(WM_KEYDOWN, key, LPARAM{1} << 30);
                if (key == 'S') { keys[VK_SHIFT] = shift; (void)SetKeyboardState(keys); }
            }
            ++dsp_script;
        }
        if (options_.test_neural && neural_script < 5 && elapsed >= 3000000 + static_cast<std::int64_t>(neural_script) * 5000000) {
            // Real key messages: F11 (fullscreen, so the picture is upscaled), then N: Neural, then N: Edge-directed, then
            // N three times: Fast, Auto, Neural again (each applied from the next converted frame), with C (guided chroma)
            // off at the second step and on again at the third, V (the split comparison) on at the first N and off at the
            // third step, then F11 back.
            if (neural_script == 2 || neural_script == 3) renderer_.Deliver(WM_KEYDOWN, 'C', 0);
            if (neural_script == 1 || neural_script == 3) renderer_.Deliver(WM_KEYDOWN, 'V', 0);
            const unsigned presses = neural_script == 3 ? 3u : 1u;
            for (unsigned k = 0; k < presses; ++k) renderer_.Deliver(WM_KEYDOWN, neural_script == 0 || neural_script == 4 ? VK_F11 : 'N', 0);
            ++neural_script;
        }
        if (options_.test_paused_quality && paused_script < 11) {
            // Event-driven: each key waits for the capture before it, each capture for the change on screen before it.
            static constexpr WPARAM kKeys[4]{'V', 'N', 'C', 'V'};
            if (paused_script == 0) { if (elapsed >= 10000000 && RequestPause(true)) ++paused_script; }
            else if (paused_script == 1) { if (elapsed >= 15000000) { renderer_.RequestCapture(); ++paused_script; } }
            else if (paused_script < 10) {
                const unsigned k = (paused_script - 2) / 2;
                if (paused_script % 2 == 0) { if (renderer_.Captures() >= k + 1) { renderer_.Deliver(WM_KEYDOWN, kKeys[k], 0); ++paused_script; } }
                else if (renderer_.PausedUpdates() >= k + 1) { renderer_.RequestCapture(); ++paused_script; }
            } else if (renderer_.Captures() >= 5 && RequestPause(false)) ++paused_script;
        }
        if (options_.test_neural_reuse && reuse_script < 5 && elapsed >= (reuse_script ? 20000000 + static_cast<std::int64_t>(reuse_script - 1) * 50000000 : 5000000)) {
            // 0.5 s: fullscreen (the picture is upscaled), Neural fixed; from 2 s (the switch settled), three phases of 5 s
            // from the start of the file, reuse Off, Exact, Tolerant, each closed by its totals. A phase counts from its seek's
            // landing (below): conversions recorded in the previous mode are executed by then (one queue, in order), so the
            // GPU's tile counters read back afterwards belong to this phase alone.
            using Reuse = render::NeuralUpscalePass::TemporalReuse;
            const auto tiles = renderer_.NeuralTiles(); const auto gpu = renderer_.NeuralGpu();
            const auto presented = static_cast<unsigned>(Presented()), dropped = static_cast<unsigned>(Dropped());
            if (reuse_script == 0) { renderer_.Deliver(WM_KEYDOWN, VK_F11, 0); renderer_.SetQualityMode(render::QualityMode::Neural); }
            else if (reuse_script > 1) {
                auto& p = neural_reuse_run_.phase[reuse_script - 2];
                p.reused = tiles.reused - reuse_tiles.reused; p.evaluated = tiles.evaluated - reuse_tiles.evaluated;
                p.gpu_sum = gpu.sum - reuse_gpu.sum; p.gpu_count = gpu.count - reuse_gpu.count;
                p.presented = presented - reuse_presented; p.dropped = dropped - reuse_dropped;
                for (unsigned k = 0; k < 4; ++k) p.dropped_by[k] = k < reuse_marks ? reuse_marked[k] - reuse_dropped : p.dropped;
            }
            if (reuse_script >= 1 && reuse_script <= 3) {
                renderer_.SetNeuralReuse(reuse_script == 1 ? Reuse::Off : reuse_script == 2 ? Reuse::Exact : Reuse::Tolerant);
                if (RequestSeek(0)) reuse_epoch = epoch_;
            }
            if (reuse_script == 4) { neural_reuse_run_.done = true; (void)RequestSeek(Bounds().duration_100ns - 10000000); } // The last second, and the end.
            ++reuse_script;
        }
        if (options_.test_neural_reuse && reuse_epoch && renderer_.LastSeek().epoch == reuse_epoch) {
            // The phase's seek landed: its count starts here.
            reuse_tiles = renderer_.NeuralTiles(); reuse_gpu = renderer_.NeuralGpu();
            reuse_presented = static_cast<unsigned>(Presented()); reuse_dropped = static_cast<unsigned>(Dropped()); reuse_epoch = 0;
            reuse_landed = Now(); reuse_marks = 0;
        }
        if (options_.test_neural_reuse && reuse_marks < 4) {
            static constexpr std::int64_t kMarks[]{5000000, 10000000, 20000000, 30000000};
            if (Now() - reuse_landed >= kMarks[reuse_marks]) reuse_marked[reuse_marks++] = static_cast<unsigned>(Dropped());
        }
        if (options_.test_quality && quality_script < 3) {
            // The governor's script (PlaybackOptions::test_quality). 37.5 ms of extra GPU work is 90 % of the 41.7 ms
            // interval at 24 fps: over the governor's 85 % threshold, under the interval itself (no picture misses its
            // own deadline because of it). Real key messages for N.
            if (quality_script == 0 && elapsed >= 40000000) { renderer_.SetTestLoad(37.5f); quality_run_.load_on = Now(); ++quality_script; }
            else if (quality_script == 1 && elapsed >= 90000000) {
                renderer_.SetTestLoad(0); quality_run_.load_off = Now(); renderer_.Deliver(WM_KEYDOWN, 'N', 0); ++quality_script;
            } else if (quality_script == 2 && elapsed >= 110000000) {
                for (unsigned k = 0; k < 3; ++k) renderer_.Deliver(WM_KEYDOWN, 'N', 0);
                quality_run_.auto_again = Now(); ++quality_script;
            }
        }
        if (options_.test_surround && surround_script < 3 && elapsed >= 8000000 + static_cast<std::int64_t>(surround_script) * 6000000) {
            // Real Shift+S key messages (S alone takes a snapshot since 2026-11-01): binaural off, on, off again (fold-down), or
            // ears on/off over passthrough; the thread's key state as the driver leaves it with Shift held.
            BYTE keys[256]{}; (void)GetKeyboardState(keys);
            const BYTE shift = keys[VK_SHIFT]; keys[VK_SHIFT] = 0x80; (void)SetKeyboardState(keys);
            renderer_.Deliver(WM_KEYDOWN, 'S', 0); ++surround_script; ++spatial_toggles_;
            keys[VK_SHIFT] = shift; (void)SetKeyboardState(keys);
        }
        if (options_.test_hud) {
            const auto& h = renderer_.HudState(); const auto time = Now();
            const float half = (h.dock_half_width - 28) * h.scale, timeline = h.center_y + h.timeline_y * h.scale;
            if (hud_script == 0 && elapsed >= 5000000) {
                renderer_.InputHud({ui::PointerEvent::Kind::Down, h.center_x, h.center_y, time});
                renderer_.InputHud({ui::PointerEvent::Kind::Up, h.center_x, h.center_y, time}); ++hud_script;
            } else if (hud_script == 1 && elapsed >= 6500000) {
                renderer_.InputHud({ui::PointerEvent::Kind::Down, h.center_x, timeline, time}); ++hud_script;
            } else if (hud_script == 2 && elapsed >= 7500000) {
                renderer_.InputHud({ui::PointerEvent::Kind::Move, h.center_x + half, timeline, time});
                renderer_.InputHud({ui::PointerEvent::Kind::Up, h.center_x + half, timeline, time}); ++hud_script;
            } else if (hud_script == 3 && elapsed >= 20000000) {
                renderer_.InputHud({ui::PointerEvent::Kind::TogglePause, 0, 0, time}); ++hud_script;
            }
        }
        if (!script_done && !script_paused && elapsed >= options_.pause_at_ms * 10000ll) script_paused = RequestPause(true);
        if (!script_done && script_paused && elapsed >= (options_.pause_at_ms + options_.pause_ms) * 10000ll) script_done = RequestPause(false);
        DrainVoice();
        DrainHostCommands(); // Embedding (API 12): the host's posted commands, before this pass shows anything.
        renderer_.PollOutputSize(); // Texture output (API 13): a size the host's renderer asked for.
        SyncAudioFocus();
        PublishHost();
        if (stop_requested_.load(std::memory_order_relaxed)) break; // A host's stop (RequestStop): as the window closing.
        if (const auto moves = endpoint_moves_.load(std::memory_order_relaxed); moves != endpoint_moves) {
            endpoint_moves = moves; Status("Audio \xc2\xb7 now on the default output device", ui::StatusIcon::Device, ui::StatusTone::Info);
        }
        bool finished = false;
        UiPhase(Phase::Stream);
        if (!Stream(finished)) break;
        if (pending_count_) FlushSignatures();
        UiPhase(Phase::Scripts);
        PumpLoop(finished);
        PumpTracking(); PumpSaccade(); PumpSnapshot();
        if (loop_.Looping()) finished = false; // A loop plays on.
        const auto now = Now();
        if (!first_frame_ && (renderer_.Presented() || renderer_.MusicPresents())) first_frame_ = now - begin; // Music mode: its first composite.
        if (finished && (!options_.test_seek || seek_results_ == 2) && (!options_.test_scrub || scrub_.done) &&
            (!options_.test_longform || (seek_results_ == 4 && results_[3].resumed)) &&
            (!options_.test_remote_seek || (seek_results_ == 5 && results_[4].resumed)) &&
            (!options_.test_resize || (resize_steps_ == 5 && resize_[4].applied)) && commands_done()) break;
        // Scripts that end before their file does.
        if (options_.test_zoom_live && zoom_script_ == 4 && renderer_.LiveZooms() && renderer_.LiveZoomAt(renderer_.LiveZooms() - 1).native) break;
        if (options_.test_telemetry && telemetry_run_.done && now - telemetry_mark_ >= 15000000) break;
        if (options_.test_bookmarks && bookmark_run_.done && now - bookmark_mark_ >= 5000000) break;
        if (options_.test_resize) {
            // Client-size changes through SetWindowPos (the same WM_SIZE path as a user drag), then F11 through the
            // production window procedure. Each step is recorded once the swap chain has been resized.
            static constexpr unsigned sizes[3][2]{{960, 540}, {540, 960}, {1600, 686}};
            if (resize_steps_ && !resize_[resize_steps_ - 1].applied && !renderer_.ResizePending() && renderer_.Presented() > resize_[resize_steps_ - 1].presented_before) {
                auto& r = resize_[resize_steps_ - 1]; const auto& h = renderer_.HudState();
                r.applied = true; r.width = renderer_.Width(); r.height = renderer_.Height(); r.video = renderer_.LastVideoRect();
                r.fullscreen = renderer_.Fullscreen(); r.hud_scale = h.scale;
                r.dock_left = h.center_x - h.dock_half_width * h.scale; r.dock_bottom = h.center_y + h.dock_half_height * h.scale;
                r.presented_after = renderer_.Presented();
            }
            if (resize_steps_ && resize_[resize_steps_ - 1].applied) resize_[resize_steps_ - 1].presented_after = renderer_.Presented();
            const bool ready = !resize_steps_ || resize_[resize_steps_ - 1].applied;
            if (ready && resize_steps_ < 5 && elapsed >= 4000000 + static_cast<std::int64_t>(resize_steps_) * 4000000) {
                auto& r = resize_[resize_steps_]; r = {}; r.presented_before = renderer_.Presented();
                if (resize_steps_ >= 3) {
                    // Step 3 enters fullscreen on the window's monitor; step 4 leaves it (back to 1600x686).
                    renderer_.Deliver(WM_KEYDOWN, VK_F11, 0);
                } else {
                    r.requested_width = sizes[resize_steps_][0]; r.requested_height = sizes[resize_steps_][1];
                    SetWindowPos(renderer_.Window(), nullptr, 0, 0, static_cast<int>(r.requested_width), static_cast<int>(r.requested_height),
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                ++resize_steps_;
            }
        }
        if (options_.test_seek || options_.test_longform || options_.test_seek_forward || options_.test_seek_micro || options_.test_remote_seek) {
            // Forward seek while playing, backward seek while paused, then resume. Each landing is recorded once
            // its first target frame is on screen; continuous playback resumes when the clock re-anchors.
            const auto& landed = renderer_.LastSeek();
            if (seek_results_ && landed.epoch == epoch_ && !results_[seek_results_ - 1].landed) {
                auto& r = results_[seek_results_ - 1];
                r.landed = true; r.pts = landed.pts; r.duration = landed.duration; r.latency = landed.latency; r.paused = paused_;
                r.subtitles = landed.subtitles;
            }
            const auto anchored = anchored_at_.load(std::memory_order_acquire);
            if (seek_results_ && results_[seek_results_ - 1].landed && !results_[seek_results_ - 1].resumed &&
                anchored > results_[seek_results_ - 1].requested) {
                results_[seek_results_ - 1].resumed = anchored;
                results_[seek_results_ - 1].audio_start = audio_start_.load(std::memory_order_acquire); // Published before the anchor.
            }
            auto seek = [&](std::int64_t target) {
                if (seek_results_ >= kMaxSeekResults) return;
                results_[seek_results_] = {}; results_[seek_results_].target = target; results_[seek_results_].requested = Now();
                if (RequestSeek(target)) { results_[seek_results_].epoch = epoch_; ++seek_results_; }
            };
            if (options_.test_seek_micro) {
                // Queue seeks (the landing among pictures already decoded), then the same as the first one restarting.
                const auto position = renderer_.MediaPosition();
                const bool moving = !paused_ && renderer_.LastSeek().epoch == epoch_ &&
                                    (seek_results_ == 0 ? clock_started_.load(std::memory_order_acquire) != 0 : results_[seek_results_ - 1].resumed != 0);
                if (seek_results_ == 0 && moving && position >= 10000000) seek(14000000);
                else if (seek_results_ == 1 && moving && position >= 16000000) seek(21000000);
                else if (seek_results_ == 2 && moving && now - results_[1].resumed >= 2000000) seek(9000000);
                else if (seek_results_ == 3 && moving && position >= 10000000) { SetQueueSeek(false); seek(14000000); }
                else if (seek_results_ == 4 && results_[3].landed) SetQueueSeek(options_.queue_seek);
            } else if (options_.test_seek_forward) {
                SetQueueSeek(false); // This test measures continuation alone: no target may be taken from the decoded queue.
                // The same forward seek twice from the same place, 1.5 s -> 2.5 s while playing: first carrying on from the
                // decode position, then (after a seek back to 1.3 s) restarting from the sync sample as before. The
                // long-GOP fixtures have a single sync sample, so both seeks stay in one group of pictures.
                const bool moving = !paused_ && renderer_.LastSeek().epoch == epoch_ && renderer_.MediaPosition() >= 15000000 &&
                                    (seek_results_ == 0 ? clock_started_.load(std::memory_order_acquire) != 0 : results_[seek_results_ - 1].resumed != 0);
                if (seek_results_ == 0 && moving) seek(25000000);
                else if (seek_results_ == 1 && results_[0].resumed && now - results_[0].resumed >= 2000000) seek(13000000);
                else if (seek_results_ == 2 && moving) { SetSeekContinuation(false); seek(25000000); }
                else if (seek_results_ == 3 && results_[2].landed) SetSeekContinuation(options_.seek_continuation);
            } else if (options_.test_remote_seek) {
                // A remote 10-minute Matroska file: far ahead and back while playing (each a cue seek: the index covers only what
                // was read), far ahead while paused, back, then the last seconds, played to the end of the stream.
                constexpr std::int64_t kSecond = 10000000;
                const auto since = [&](unsigned i) { return results_[i].resumed ? now - results_[i].resumed : -1; };
                if (seek_results_ == 0 && elapsed >= 15 * kSecond / 10) seek(400 * kSecond + kSecond / 2);
                else if (seek_results_ == 1 && since(0) >= kSecond) seek(120 * kSecond + kSecond / 4);
                else if (seek_results_ == 2 && since(1) >= kSecond) { if (RequestPause(true)) seek(530 * kSecond + 4 * kSecond / 10); }
                else if (seek_results_ == 3 && results_[2].landed && paused_) (void)RequestPause(false);
                else if (seek_results_ == 3 && since(2) >= kSecond) seek(121 * kSecond + 7 * kSecond / 10);
                else if (seek_results_ == 4 && since(3) >= kSecond) seek(595 * kSecond + 3 * kSecond / 10);
            } else if (options_.test_longform) {
                // 1 h and 2 h while playing, 45 min while paused, then 3 h 30 min, playing on to the end of the stream.
                constexpr std::int64_t kSecond = 10000000;
                const auto since = [&](unsigned i) { return results_[i].resumed ? now - results_[i].resumed : -1; };
                if (seek_results_ == 0 && elapsed >= 15 * kSecond / 10) seek(3600 * kSecond + kSecond / 2);
                else if (seek_results_ == 1 && since(0) >= kSecond) seek(7200 * kSecond + kSecond / 4);
                else if (seek_results_ == 2 && since(1) >= kSecond) { if (RequestPause(true)) seek(2700 * kSecond + 4 * kSecond / 10); }
                else if (seek_results_ == 3 && results_[2].landed && paused_) (void)RequestPause(false);
                else if (seek_results_ == 3 && since(2) >= kSecond) seek(12600 * kSecond + 3 * kSecond / 10);
            } else if (seek_results_ == 0 && elapsed >= 8000000) seek(17000000); // Forward, playing: 0.8 s -> 1.7 s.
            else if (seek_results_ == 1 && results_[0].resumed && elapsed >= 14000000) {
                if (RequestPause(true)) seek(6000000); // Backward, paused: 0.6 s. The target frame must still appear.
            } else if (seek_results_ == 2 && results_[1].landed && paused_ && elapsed >= 20000000) (void)RequestPause(false);
        }
        PumpScrub();
        if (options_.test_scrub) {
            // A 120 Hz scrub drag: 0.5 s -> 2.4 s over 0.5 s, back to 1.1 s over 0.25 s, then release at 1.1 s.
            // Then a burst of four immediate seeks committed while the worker holds its queue: it must drain them
            // together and apply only the last.
            const auto& landed = renderer_.LastSeek();
            if (scrub_phase == 1 && landed.epoch != seen_epoch && landed.epoch > 1) {
                // A frame shown while dragging, counted in the half of the drag it arrived in (the backward half starts at
                // 0.5 s), with the wait since the previous one.
                if (scrub_.landings < ScrubResult::kMaxTrace)
                    scrub_.trace[scrub_.landings] = {landed.presented - drag_begin, landed.latency, landed.pts, seek_target_, Preview(landed.epoch)};
                seen_epoch = landed.epoch; ++scrub_.landings; if (Preview(landed.epoch)) ++scrub_.previews;
                const auto gap = landed.presented - last_landing; last_landing = landed.presented;
                scrub_.max_gap = (std::max)(scrub_.max_gap, gap);
                if (landed.presented - drag_begin < 5000000) { ++scrub_.forward_landings; scrub_.forward_gap = (std::max)(scrub_.forward_gap, gap); }
                else { ++scrub_.backward_landings; scrub_.backward_gap = (std::max)(scrub_.backward_gap, gap); }
            }
            if (scrub_phase == 0 && elapsed >= 5000000) { scrub_phase = 1; drag_begin = last_landing = now; }
            else if (scrub_phase == 1 && now - last_event >= 83333) {
                const auto t = now - drag_begin; std::int64_t target = 11000000;
                if (t < 5000000) target = 5000000 + t * 19 / 5;               // Forward: 0.5 -> 2.4 s.
                else if (t < 7500000) target = 24000000 - (t - 5000000) * 26 / 5;  // Backward: 2.4 -> 1.1 s.
                else scrub_phase = 2;
                if (scrub_phase == 2) {
                    // Release at the last target, as the dock's timeline does (HudCommand::Seek on button up): one plain seek,
                    // exact with its audio, unless the last scrub seek already landed there exactly.
                    const auto sent = seeks_; scrub_.target = target;
                    if (RequestSeek(target)) { scrub_.released = seeks_ != sent; last_request = now; }
                } else {
                    // The first event arrives as three requests at once (an input backlog): the first is sent, the second
                    // must be replaced in the pending slot by the third, so coalescing is exercised on fast decoders too.
                    for (unsigned k = last_event ? 1u : 3u; k != 0; --k)
                        if (RequestSeek(target - (k - 1) * 100000, true)) { ++scrub_.requests; last_request = now; }
                    last_event = now; scrub_.target = target;
                }
            } else if (scrub_phase == 2 && !scrub_pending_ && landed.epoch == epoch_) {
                scrub_.pts = landed.pts; scrub_.duration = landed.duration; scrub_.final_latency = landed.presented - last_request;
                scrub_.subtitles = landed.subtitles;
                scrub_phase = 3; phase_time = landed.presented;
            } else if (scrub_phase == 3 && anchored_at_.load(std::memory_order_acquire) > phase_time && now - phase_time >= 3000000) {
                hold_controls_.store(true, std::memory_order_release);
                const auto superseded = superseded_seeks_.load();
                bool sent = true;
                for (const std::int64_t target : {19000000ll, 4000000ll, 22000000ll, 12900000ll}) sent = RequestSeek(target) && sent;
                hold_controls_.store(false, std::memory_order_release);
                scrub_.burst_target = 12900000; burst_epoch = epoch_; burst_before = superseded;
                scrub_phase = sent ? 4 : 99;
            } else if (scrub_phase == 4 && landed.epoch == burst_epoch) {
                scrub_.burst_pts = landed.pts; scrub_.burst_duration = landed.duration; phase_time = landed.presented; scrub_phase = 5;
                scrub_.burst_subtitles = landed.subtitles;
            } else if (scrub_phase == 5 && anchored_at_.load(std::memory_order_acquire) > phase_time) {
                scrub_.burst_superseded = superseded_seeks_.load() - burst_before;
                scrub_.burst_audio = audio_start_.load(std::memory_order_acquire); scrub_.done = true; scrub_phase = 6;
            }
        }
        if (options_.maximum_seconds && now - begin > static_cast<std::int64_t>(options_.maximum_seconds) * 10000000) { Fail(27); break; }
        if (!cpu_counting_ && clock_started_.load(std::memory_order_relaxed)) { cpu_counting_ = true; AccountCpu(); } // Shares from the clock's start.
        // The scripts' next deadline, which a settled pause sleeps no later than (Pace): the next scripted command or typing,
        // the scripted pause's resume, the run's time cap.
        script_wake_ = INT64_MAX;
        const auto at = [&](std::int64_t ms) { script_wake_ = (std::min)(script_wake_, begin + ms * 10000); };
        if (command_results_ < command_count && !options_.test_chapters) at(options_.commands[command_results_].at_ms);
        if (typed < typing_count) at(options_.typing[typed].at_ms);
        if (!script_done) at(script_paused ? options_.pause_at_ms + options_.pause_ms : options_.pause_at_ms);
        if (options_.maximum_seconds) at(std::int64_t{options_.maximum_seconds} * 1000 + 1);
        Pace();
    }
    UiPhase(Phase::Stopping);
    if (cpu_counting_) AccountCpu();
    KeepAwake(false);
    stop_.store(true);
    // The worker leaves its loop within one pass. The wait is bounded all the same (docs/session.md, "Watchdog"): a worker
    // that does not stop within 10 s is reported (stage 36) instead of holding the caller without a word.
    for (const auto asked = Now(); !stopped_.load();) {
        if (Now() - asked > 100000000) { Fail(36); break; }
        if (!SwitchToThread()) YieldProcessor();
    }
    Remember();
    {
        // Embedding (API 12): the last event, and why Run returns.
        const auto error = error_.load();
        HostEvent ended{}; ended.kind = HostEvent::kEnded; ended.time = Now(); ended.position = PrecisePosition(); ended.duration = duration_;
        ended.reason = error == 27 ? HostEvent::kLimit : error ? HostEvent::kError : stop_requested_.load(std::memory_order_relaxed) ? HostEvent::kStopped :
                       renderer_.Closed() ? HostEvent::kClosed : HostEvent::kMedia;
        ended.status = error && error != 27 ? 1 : 0;
        Emit(ended);
    }
    return error_.load() == 0;
}
void PlaybackCoordinator::StartCue() noexcept {
    // "2 of 5 · Episode 2.mp4", "Resuming at 1:02:03 · Home: from the start", or both ("2 of 5 · Episode 2.mp4 · from 1:02:03").
    const unsigned files = options_.files_before + options_.files_after;
    if (!files && !resume_.resumed) return;
    char* text = resume_.cue; unsigned n = 0; const unsigned size = sizeof(resume_.cue);
    const auto put = [&](const char* part) { while (*part && n + 1 < size) text[n++] = *part++; };
    const auto number = [&](std::uint64_t v, unsigned digits) {
        char d[24]; unsigned k = 0; do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while ((v || k < digits) && k < 23);
        while (k && n + 1 < size) text[n++] = d[--k];
    };
    const auto clock = [&](std::int64_t t) {
        const auto s = static_cast<std::uint64_t>(t > 0 ? t / 10000000 : 0);
        if (s >= 3600) { number(s / 3600, 1); put(":"); number(s / 60 % 60, 2); } else number(s / 60, 1);
        put(":"); number(s % 60, 2);
    };
    if (files) {
        number(options_.files_before + 1u, 1); put(" of "); number(files + 1u, 1);
        if (file_name_[0]) { put(" \xc2\xb7 "); put(file_name_); }
        if (resume_.resumed) { put(" \xc2\xb7 from "); clock(resume_.target); }
    } else { put("Resuming at "); clock(resume_.target); put(" \xc2\xb7 Home: from the start"); }
    text[n] = 0;
    Status(text, ui::StatusIcon::None, ui::StatusTone::Info, 3.0f);
}
void PlaybackCoordinator::Remember() noexcept {
    // Where this file was left, and its tracks: read again when it is opened next (docs/session.md).
    if (!options_.resume || !resume_key_) return;
    session::ResumeEntry e{};
    e.key = resume_key_; e.file_bytes = resume_bytes_;
    e.position = (std::max)(Position(), std::int64_t{0});
    e.duration = video_.duration_100ns > 0 ? video_.duration_100ns : duration_;
    e.audio_track = audio_choice_ == kNoTrack ? 0u : audio_choice_; e.subtitle_track = subtitle_choice_;
    bookmarks_.Store(e.bookmark_count, e.bookmarks);
    options_.resume->Record(e);
    resume_.recorded = true; resume_.written = e;
}
bool PlaybackCoordinator::SetSpeed(float speed) noexcept {
    if (!(speed >= audio::TimeStretcher::kMinSpeed && speed <= audio::TimeStretcher::kMaxSpeed) || !controls_) return false;
    const auto thousandths = static_cast<std::uint32_t>(std::lround(speed * 1000.0f));
    if (thousandths != static_cast<std::uint32_t>(std::lround(speed_ * 1000.0f))) {
        auto* command = controls_->AcquireWriteSlot();
        if (!command) return false;
        *command = {}; command->kind = ControlCommand::Kind::Speed; command->epoch = thousandths;
        if (!CommitControl()) return false;
        speed_ = static_cast<float>(thousandths) / 1000.0f;
        ui_events_.Record(Now(), core::FlightEvent::Speed, 0, thousandths);
    }
    // "Speed · 1.5×" (the multiplication sign), assembled in place.
    char text[32]; unsigned n = 0;
    const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; };
    put("Speed \xc2\xb7 ");
    const unsigned whole = thousandths / 1000, fraction = thousandths % 1000;
    text[n++] = static_cast<char>('0' + whole);
    if (fraction) { text[n++] = '.'; char d[3]{static_cast<char>('0' + fraction / 100), static_cast<char>('0' + fraction / 10 % 10), static_cast<char>('0' + fraction % 10)};
                    unsigned k = 3; while (k > 1 && d[k - 1] == '0') --k; for (unsigned i = 0; i < k; ++i) text[n++] = d[i]; }
    put("\xc3\x97"); text[n] = 0;
    Status(text, ui::StatusIcon::None, ui::StatusTone::Info, 1.5f);
    return true;
}
bool PlaybackCoordinator::StepSpeed(int direction) noexcept {
    // The next step of the list from the speed now (a speed between two steps goes to the nearer one that way).
    float next = speed_;
    if (direction > 0) { for (const float s : kSpeeds) if (s > speed_ + 0.001f) { next = s; break; } }
    else for (unsigned i = static_cast<unsigned>(std::size(kSpeeds)); i-- > 0;) if (kSpeeds[i] < speed_ - 0.001f) { next = kSpeeds[i]; break; }
    if (next == speed_) { Status(direction > 0 ? "Speed \xc2\xb7 2\xc3\x97 is the fastest" : "Speed \xc2\xb7 0.25\xc3\x97 is the slowest", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f); return false; }
    return SetSpeed(next);
}
bool PlaybackCoordinator::StepFrame(int direction) noexcept {
    if (music_) return false; // No pictures to step through.
    if (!controls_ || !seekable_ || renderer_.ShownPts() == INT64_MIN) return false;
    if (!paused_ && !RequestPause(true)) return false;
    pending_steps_ += direction > 0 ? 1 : -1;
    PumpSteps();
    return true;
}
void PlaybackCoordinator::PumpSteps() noexcept {
    // One step at a time, each from the picture the previous one landed on.
    if (step_epoch_) { if (renderer_.LastSeek().epoch != step_epoch_) return; ++frame_steps_; step_epoch_ = 0; }
    if (!pending_steps_) return;
    const auto pts = renderer_.ShownPts(), duration = renderer_.ShownDuration();
    if (pts == INT64_MIN) return;
    const bool forward = pending_steps_ > 0;
    const auto target = forward ? pts + (duration > 0 ? duration : 1) : pts - 1;
    const auto limit = video_.duration_100ns > 0 ? video_.duration_100ns : duration_;
    if (target < 0 || (limit > 0 && target >= limit)) { pending_steps_ = 0; Status(forward ? "Last picture" : "First picture", ui::StatusIcon::None, ui::StatusTone::Info, 1.0f); return; }
    if (!RequestSeek(target)) return;
    pending_steps_ += forward ? -1 : 1; step_epoch_ = epoch_;
}
ai::IntentStatus PlaybackCoordinator::RequestFile(int direction) noexcept {
    const bool next = direction > 0;
    if (!(next ? options_.files_after : options_.files_before)) {
        Status(next ? "No next video in this folder" : "No previous video in this folder", ui::StatusIcon::Warning, ui::StatusTone::Warning);
        return ai::IntentStatus::NotFound;
    }
    file_request_ = next ? FileRequest::Next : FileRequest::Previous;
    return ai::IntentStatus::Ok;
}
void PlaybackCoordinator::LoseGpu(unsigned detector, HRESULT hr) noexcept {
    std::uint32_t running = kGpuRunning;
    if (!gpu_phase_.compare_exchange_strong(running, kGpuLost, std::memory_order_acq_rel)) return; // Already recovering.
    // Read by the presentation thread once the worker has parked (its release publishes these stores).
    gpu_detector_.store(detector, std::memory_order_relaxed); gpu_reason_.store(hr, std::memory_order_relaxed);
    // Each thread writes its own recorder.
    (detector == 2 ? worker_events_ : ui_events_).Record(Now(), core::FlightEvent::DeviceLost, static_cast<std::uint16_t>(detector), static_cast<std::uint32_t>(hr));
}
bool PlaybackCoordinator::RecoverGpu() noexcept {
    const auto phase = gpu_phase_.load(std::memory_order_acquire);
    const auto now = Now();
    if (phase == kGpuRunning) { recovering_ = false; return true; }
    if (!recovering_) {
        recovering_ = true; recovery_ = {}; recovery_.lost = now; recovery_.shown = renderer_.ShownPts(); recovery_.paused = paused_;
        recovery_deadline_ = now + 100000000; recovery_retry_ = 0; // A driver restart gets 10 s.
    }
    if (phase != kGpuParked || now < recovery_retry_) {
        if (now < recovery_deadline_) return true;
        Fail(34, recovery_.reason); return false;
    }
    // Parked: the worker no longer touches the decoder, the frame queue or the surface returns.
    if (!recovery_.attempts) {
        recovery_.parked = now;
        recovery_.detector = gpu_detector_.load(std::memory_order_relaxed);
        recovery_.reason = renderer_.Device() ? renderer_.Device()->GetDeviceRemovedReason() : S_OK;
        if (SUCCEEDED(recovery_.reason)) recovery_.reason = gpu_reason_.load(std::memory_order_relaxed);
        recovery_.subtitle_presents = renderer_.SubtitlePresents();
        // Frames decoded on the lost device: their surfaces no longer exist, so nothing is returned.
        while (frames_->PeekReadSlot()) (void)frames_->ReleaseReadSlot();
        if (!music_) decoder_.Suspend();
        // A snapshot being written reads the readback the release frees: it is finished first (a write of a few milliseconds;
        // recovery only). One still being copied is lost with the device.
        while (snapshot_phase_.load(std::memory_order_acquire) == 2 || snapshot_phase_.load(std::memory_order_acquire) == 4) Sleep(1);
        const int snapshot = snapshot_phase_.load(std::memory_order_acquire);
        if (snapshot == 1 || snapshot == 5) snapshot_phase_.store(0, std::memory_order_release);
        if (!renderer_.ReleaseDevice(recovery_.leaked)) { Fail(34, renderer_.LastError()); return false; }
        recovery_.released = Now();
    }
    ++recovery_.attempts;
    bool rebuilt = renderer_.RecreateDevice();
    HRESULT hr = renderer_.LastError();
    recovery_.recreated = Now();
    if (rebuilt && !music_ && decoder_.Resume(renderer_.Device(), renderer_.ConsumerFence(), worker_epoch_.load(std::memory_order_acquire)) != DecodeStatus::Ready) {
        rebuilt = false; hr = decoder_.LastError();
    }
    if (rebuilt && !music_ && !renderer_.Bind(decoder_)) { rebuilt = false; hr = E_FAIL; }
    if (!rebuilt) {
        // A restarting driver can refuse a device for a while: release what was built and try again shortly.
        if (!music_) decoder_.Suspend();
        ULONG remaining = 0; (void)renderer_.ReleaseDevice(remaining);
        if (now >= recovery_deadline_) { Fail(34, hr); return false; }
        recovery_retry_ = now + 5000000; return true;
    }
    // Returns still queued for the worker belong to the lost device: they are dropped, never validated.
    retired_ = validated_frames_.load();
    // Resume at the picture on screen when paused, at the clock when playing, or (a seek's first frame, or the
    // stream's, still due) at the epoch's own target.
    std::int64_t target = INT64_MIN; bool running = false;
    if (!renderer_.PreviewPending() && renderer_.Epoch() == worker_epoch_.load(std::memory_order_acquire)) {
        clock::ClockEstimate estimate{};
        running = !paused_ && clock_.Estimate(Now(), estimate) == clock::ClockQuery::Ready && estimate.epoch == renderer_.Epoch();
        target = running ? estimate.media_time_100ns : recovery_.shown;
        // Past the end (a long rebuild near it), the last frame is still shown, as for a seek.
        if (video_.duration_100ns > 0) target = (std::min)(target, video_.duration_100ns - 1);
    }
    recovery_.target = target; recovery_.rebuilt = Now(); ++device_recoveries_;
    ui_events_.Record(recovery_.rebuilt, core::FlightEvent::DeviceRecovered, 0, device_recoveries_, recovery_.rebuilt - recovery_.lost);
    recovery_target_.store(target, std::memory_order_relaxed); recovery_catch_up_.store(running, std::memory_order_relaxed);
    gpu_phase_.store(kGpuResume, std::memory_order_release);
    return true;
}
bool PlaybackCoordinator::Stream(bool& finished) noexcept {
    finished = false;
    if (streaming_) return true;
    streaming_ = true;
    if (recovering_ || gpu_phase_.load(std::memory_order_acquire) != kGpuRunning) {
        const bool recovered = RecoverGpu(); streaming_ = false; return recovered;
    }
    const auto no_display = static_cast<unsigned>(core::VideoFrameFlags::NoDisplay);
    if (adopt_wait_ && worker_epoch_.load(std::memory_order_acquire) >= adopt_wait_) {
        renderer_.ResolveAdoption(adopted_epoch_.load(std::memory_order_relaxed) == adopt_wait_); adopt_wait_ = 0;
    }
    bool ok = true;
    if (const auto* frame = frames_->PeekReadSlot()) {
        auto* returned = surface_returns_->AcquireWriteSlot();
        if (returned) {
            if (static_cast<unsigned>(frame->flags) & no_display) {
                // Reference-only picture: retired at once without GPU access, never presented.
                *returned = {frame->surface_id, frame->surface_generation, frame->playback_epoch, 2, 0};
                (void)surface_returns_->CommitWriteSlot(); WakeWorker(); (void)frames_->ReleaseReadSlot(); ++hidden_; ++retired_;
            } else {
                const auto before = Now();
                UiPhase(Phase::Convert);
                const auto state = renderer_.Convert(*frame, decoder_, *returned);
                UiPhase(Phase::Stream);
                const auto took = Now() - before;
                if (clock_started_.load(std::memory_order_relaxed)) { stalls_.convert_sum += took; ++stalls_.converts; }
                if (took > stalls_.convert && clock_started_.load(std::memory_order_relaxed)) { stalls_.convert = took; stalls_.convert_pts = renderer_.ShownPts(); }
                if (state == render::RenderStatus::Ready) { (void)surface_returns_->CommitWriteSlot(); WakeWorker(); (void)frames_->ReleaseReadSlot(); ++retired_; }
                else {
                    (void)surface_returns_->CancelWriteSlot();
                    if (state == render::RenderStatus::DeviceLost) LoseGpu(1, renderer_.LastError());
                    else if (state == render::RenderStatus::Failed) { Fail(25, renderer_.LastError()); ok = false; }
                }
            }
        }
    }
    renderer_.SetVolume(Volume(), Muted()); // The dock's volume pill follows the engine, whoever changed it.
    if (audio_pictures_) PumpSound();
    const auto now = Now();
    // Once input has ended, frames below the reorder depth may leave the window. End of input only counts
    // for the newest epoch: the worker resets it before publishing a completed seek.
    const bool current = worker_epoch_.load(std::memory_order_acquire) == epoch_;
    const bool draining = current && input_done_.load() && !frames_->PeekReadSlot();
    // A scrub preview's picture has no successors: once it is converted, it may leave the reorder window at once.
    const bool held = current && preview_held_.load(std::memory_order_acquire) == epoch_ && !frames_->PeekReadSlot();
    // A picture setting changed while paused, and the renderer does not hold the picture on screen: that picture again, decoded
    // and converted with the new settings (a seek to it; the renderer keeps its source from then on).
    if (ok && paused_ && renderer_.TakeRefresh() && renderer_.ShownPts() != INT64_MIN && SendSeek(renderer_.ShownPts(), false)) ++paused_refreshes_;
    UiPhase(Phase::Present);
    const auto presented = ok ? renderer_.Present(clock_, now, {draining || held, paused_}) : render::RenderStatus::Failed;
    UiPhase(Phase::Stream);
    const auto present_took = Now() - now;
    if (clock_started_.load(std::memory_order_relaxed)) { stalls_.present_sum += present_took; ++stalls_.presents; }
    if (present_took > stalls_.present && clock_started_.load(std::memory_order_relaxed)) { stalls_.present = present_took; stalls_.present_pts = renderer_.ShownPts(); }
    if (presented == render::RenderStatus::DeviceLost) LoseGpu(1, renderer_.LastError());
    else if (ok && presented == render::RenderStatus::Failed) { Fail(26, renderer_.LastError()); ok = false; }
    // The epoch whose first picture has been handed to the swap chain (the landing of a seek; epoch 1 is the start).
    if (renderer_.LastSeek().epoch > ready_epoch_.load(std::memory_order_relaxed)) ready_epoch_.store(renderer_.LastSeek().epoch, std::memory_order_release);
    // Cuts seen while playing join the timeline's scenes, and the dock's marks.
    std::int64_t cut = 0; bool marked = false;
    while (renderer_.NextCut(cut_cursor_, cut)) {
        const auto scenes = timeline_.Scenes(), live = timeline_.LiveScenes();
        (void)timeline_.AddScene(cut, timeline::SceneOrigin::Live);
        marked = marked || timeline_.Scenes() != scenes || timeline_.LiveScenes() != live;
    }
    if (marked) PackMarks();
    NoteEvents();
    // Honest preview state: the dock marks the picture on screen as a keyframe preview until an exact landing replaces it,
    // and the first preview of a drag says so once.
    const bool preview = Preview(renderer_.LastSeek().epoch);
    if (preview != showing_preview_) { showing_preview_ = preview; renderer_.SetHudPreview(preview); }
    if (preview && !preview_cued_) { preview_cued_ = true; Status("Keyframe preview \xc2\xb7 the exact frame follows on release", ui::StatusIcon::None, ui::StatusTone::Info, 1.5f); }
    if (ok && draining && !paused_ && renderer_.Empty() && validated_frames_.load() == retired_ &&
        gpu_phase_.load(std::memory_order_relaxed) == kGpuRunning) {
        clock::ClockEstimate estimate{};
        finished = clock_.Estimate(now, estimate) == clock::ClockQuery::Ready && estimate.epoch == epoch_ && estimate.media_time_100ns >= audio_end_.load();
    }
    streaming_ = false;
    return ok;
}
void PlaybackCoordinator::PumpSound() noexcept {
    // Music mode: the epoch lands once its sound plays (no picture will), and the renderer is handed the latest picture of
    // the blocks heard by the time its composite reaches the screen (a refresh from now: the display paces music mode's
    // composites, one frame queued). Pictures of an epoch a seek has left go; while paused, or before the clock runs, none
    // is due.
    clock::ClockEstimate e{};
    const bool timed = !paused_ && clock_.Estimate(Now(), e) == clock::ClockQuery::Ready && e.epoch == epoch_;
    if (timed && renderer_.LastSeek().epoch < epoch_ && renderer_.Epoch() == epoch_) renderer_.LandAudio(e.media_time_100ns);
    const auto shown_at = e.media_time_100ns + static_cast<std::int64_t>(static_cast<double>(renderer_.RefreshPeriod()) * e.speed);
    auto& m = music_run_;
    audio::AudioPicture latest; bool have = false;
    while (const auto* p = audio_pictures_->PeekReadSlot()) {
        if (p->epoch < epoch_) { (void)audio_pictures_->ReleaseReadSlot(); ++m.stale; continue; }
        if (!timed || p->media_100ns > shown_at) break;
        if (have) ++m.skipped;
        latest = *p; have = true; (void)audio_pictures_->ReleaseReadSlot();
    }
    if (!have) return;
    renderer_.ShowAudio(latest); ++m.shown;
    const auto lag = shown_at - latest.media_100ns; // From the block's start to the screen's expected time.
    m.lag_min = (std::min)(m.lag_min, lag); m.lag_max = (std::max)(m.lag_max, lag); m.lag_sum += lag;
}
void PlaybackCoordinator::Pump(void* context) noexcept {
    // Modal move/size loop (WM_TIMER): keep converting and presenting until nothing more is ready this tick.
    auto& self = *static_cast<PlaybackCoordinator*>(context);
    bool finished = false;
    for (unsigned i = 0; i < 8 && !self.error_.load() && self.Stream(finished); ++i) if (!self.frames_->PeekReadSlot()) break;
}
bool PlaybackCoordinator::RequestPause(bool pause) noexcept {
    if (!controls_ || pause == paused_) return controls_ != nullptr;
    auto* command = controls_->AcquireWriteSlot();
    if (!command) return false;
    *command = {}; command->kind = pause ? ControlCommand::Kind::Pause : ControlCommand::Kind::Resume;
    if (!CommitControl()) return false;
    if (cpu_counting_) AccountCpu(); // The time until now to the state it was spent in.
    paused_ = pause; renderer_.SetHudPaused(pause); KeepAwake(!pause);
    ui_events_.Record(Now(), pause ? core::FlightEvent::Pause : core::FlightEvent::Resume, 0, 0, Position());
    return true;
}
const MediaTrack* PlaybackCoordinator::FindTrack(std::uint32_t id, demux::TrackKind kind) const noexcept {
    for (unsigned c = 0; c < track_count_; ++c) if (tracks_[c].id == id && tracks_[c].kind == kind) return &tracks_[c];
    return nullptr;
}
bool PlaybackCoordinator::SelectAudioTrack(std::uint32_t id) noexcept {
    const auto* track = FindTrack(id, demux::TrackKind::Audio);
    if (!track || track->use != TrackUse::Selectable || !controls_) return false;
    if (id == audio_choice_) { TrackCue(track, true); return true; }
    auto* command = controls_->AcquireWriteSlot();
    if (!command) return false;
    *command = {}; command->kind = ControlCommand::Kind::SelectAudio;
    command->epoch = static_cast<std::uint32_t>(track - tracks_); command->target_100ns = Now();
    if (!CommitControl()) return false;
    audio_choice_ = id; TrackCue(track, true);
    return true;
}
bool PlaybackCoordinator::SelectSubtitleTrack(std::uint32_t id) noexcept {
    if (id == kNoTrack) {
        const_cast<subtitle::SubtitleTrack&>(Captions()).Show(0); subtitle_choice_ = kNoTrack; TrackCue(nullptr, false);
        return true;
    }
    // An attached file (API 17) is the sidecar, id 0, whether or not the file had one at open.
    if (id == 0 && live_track_.load(std::memory_order_acquire) != &subtitles_) {
        const_cast<subtitle::SubtitleTrack&>(Captions()).Show(1u << attached_slot_); subtitle_choice_ = subtitle_last_ = 0;
        TrackCue(nullptr, false);
        return true;
    }
    const auto* track = FindTrack(id, demux::TrackKind::Subtitle);
    if (!track || track->use != TrackUse::Selectable) return false;
    // One store: the renderer selects, draws and prefetches only the channel shown from its next update, and the quote
    // search looks there from now on.
    const_cast<subtitle::SubtitleTrack&>(Captions()).Show(1u << track->slot); subtitle_choice_ = subtitle_last_ = id;
    TrackCue(track, false);
    return true;
}
bool PlaybackCoordinator::RequestSeek(std::int64_t target, bool scrub) noexcept {
    if (!controls_ || !seekable_) return false;
    PumpScrub();
    if (scrub) {
        // The drag's direction, from its own requests: the targets sent lag it (coalescing), and the picture on screen
        // may be a preview behind it.
        if (scrubbing_ && target != scrub_request_) scrub_forward_ = target > scrub_request_;
        else if (!scrubbing_) { scrub_forward_ = renderer_.ShownPts() == INT64_MIN || target > renderer_.ShownPts(); scrub_from_ = Position(); }
        scrubbing_ = true; last_scrub_ = Now(); scrub_request_ = target;
    }
    if (scrub && in_flight_) { // Newest target waits in the single pending slot.
        if (scrub_pending_) ++coalesced_scrubs_;
        scrub_target_ = target; scrub_pending_ = true; return true;
    }
    if (scrub_pending_) { ++coalesced_scrubs_; scrub_pending_ = false; } // Superseded by this seek.
    if (!scrub) {
        // A jump: "undo" goes back to where playback was (for a drag, where it began).
        const bool release = scrubbing_; scrubbing_ = preview_cued_ = false;
        const auto from = release ? scrub_from_ : Position();
        // A drag's release on the target its last scrub seek already landed on exactly (not a preview): nothing to do.
        if (release && epoch_ > 1 && SeekLimit(target) == seek_target_ && worker_epoch_.load(std::memory_order_acquire) >= epoch_ && !Preview(epoch_)) {
            conversation_.NoteJump(from); return true;
        }
        if (!SendSeek(target, false)) return false;
        conversation_.NoteJump(from); return true;
    }
    return SendSeek(target, scrub);
}
void PlaybackCoordinator::PumpScrub() noexcept {
    // A seek stops being in flight once its first frame is on screen (or after 1 s, so a seek that can never
    // land cannot stall scrubbing).
    if (in_flight_ && (renderer_.LastSeek().epoch == epoch_ || Now() - sent_at_ >= 10000000)) in_flight_ = false;
    if (scrub_pending_ && !in_flight_ && SendSeek(scrub_target_, true)) scrub_pending_ = false;
    // A drag held still on a keyframe preview: the exact frame (and its audio) follows after kScrubSettle, as on release.
    if (scrubbing_ && !in_flight_ && !scrub_pending_ && renderer_.LastSeek().epoch == epoch_ && Preview(epoch_) &&
        Now() - last_scrub_ >= kScrubSettle && SendSeek(seek_target_)) ++settled_scrubs_;
}
std::int64_t PlaybackCoordinator::SeekLimit(std::int64_t target) const noexcept {
    // A target at the very end still lands on the last video frame (its interval contains the video duration - 1),
    // even when the audio track runs longer: a seek must always have a frame to show.
    const auto limit = video_.duration_100ns > 0 ? video_.duration_100ns : duration_;
    return (std::clamp)(target, std::int64_t{0}, limit > 0 ? limit - 1 : std::int64_t{0});
}
const PlaybackCoordinator::SeekWork* PlaybackCoordinator::WorkFor(std::uint32_t epoch) const noexcept {
    for (unsigned i = work_count_.load(std::memory_order_acquire); i-- > 0;) if (work_[i].epoch == epoch) return &work_[i];
    return nullptr;
}
bool PlaybackCoordinator::SendSeek(std::int64_t target, bool scrub) noexcept {
    if (epoch_ == UINT32_MAX) return false;
    target = SeekLimit(target);
    auto* command = controls_->AcquireWriteSlot();
    if (!command) return false;
    *command = {}; command->kind = ControlCommand::Kind::Seek; command->epoch = epoch_ + 1; command->target_100ns = target;
    // Forward of both the picture on screen and the newest target: the pictures already decoded may hold it (a queue
    // seek). Earlier frames leave presentation now, before the worker even wakes; with a queue seek, the later ones stay.
    const auto shown = renderer_.ShownPts();
    const bool forward = queue_seek_.load(std::memory_order_relaxed) && shown != INT64_MIN && target > shown && target > seek_target_ &&
                         gpu_phase_.load(std::memory_order_acquire) == kGpuRunning;
    sent_at_ = Now();
    const bool kept = renderer_.BeginEpoch(epoch_ + 1, sent_at_, target, forward, chain_epoch_.load(std::memory_order_acquire));
    // Scrub preview (bit 2): the worker may show the target's sync sample alone; bit 3: the drag moves forward.
    const bool preview = scrub && scrub_preview_.load(std::memory_order_relaxed);
    command->reserved[0] = (forward ? (kept ? 3u : 1u) : 0u) | (preview ? 4u : 0u) | (preview && scrub_forward_ ? 8u : 0u);
    if (!CommitControl()) return false;
    ++epoch_; ++seeks_; in_flight_ = true; seek_target_ = target;
    ui_events_.Record(sent_at_, core::FlightEvent::SeekRequested, static_cast<std::uint16_t>(scrub ? 1u : forward ? 2u : 0u), epoch_, target);
    adopt_wait_ = forward ? epoch_ : 0; // Also a kept one: a declined adoption must drop what was kept.
    return true;
}
bool PlaybackCoordinator::HandleHud(void* context, ui::HudAction action) noexcept {
    auto& self = *static_cast<PlaybackCoordinator*>(context);
    if (action.command == ui::HudCommand::Pause) return self.RequestPause(true);
    if (action.command == ui::HudCommand::Resume) return self.RequestPause(false);
    if (action.command == ui::HudCommand::Seek) return self.RequestSeek(action.seek_target);
    if (action.command == ui::HudCommand::Scrub) return self.RequestSeek(action.seek_target, true); // A timeline drag moving.
    if (action.command == ui::HudCommand::Chapter) return self.Navigate(true, action.seek_target > 0 ? 1 : -1) == ai::IntentStatus::Ok;
    if (action.command == ui::HudCommand::File) return self.RequestFile(action.seek_target > 0 ? 1 : -1) == ai::IntentStatus::Ok;
    if (action.command == ui::HudCommand::Speed) return self.StepSpeed(action.seek_target > 0 ? 1 : -1);
    if (action.command == ui::HudCommand::Step) return self.StepFrame(action.seek_target > 0 ? 1 : -1);
    if (action.command == ui::HudCommand::Loop) return self.ToggleLoop() == ai::IntentStatus::Ok;
    if (action.command == ui::HudCommand::Night) return self.SetNight(!self.Night());
    if (action.command == ui::HudCommand::Bookmark) return (action.seek_target ? self.PinFrame() : self.AddBookmark()) == ai::IntentStatus::Ok;
    if (action.command == ui::HudCommand::Track) { (void)self.Track(!self.renderer_.Tracking()); return true; }
    if (action.command == ui::HudCommand::NextTarget) { (void)self.NextTarget(); return true; }
    if (action.command == ui::HudCommand::Snapshot) { (void)self.Snapshot(); return true; } // S (docs/picture.md, "Forensic snapshots").
    if (action.command == ui::HudCommand::Stats) {
        // I toggles the panel; with another cue over it (it stopped refreshing), I shows it again.
        const bool up = self.stats_shown_ && self.renderer_.Status().Revision() == self.stats_revision_;
        return self.ShowStats(!up) == ai::IntentStatus::Ok;
    }
    if (action.command == ui::HudCommand::Volume) {
        // Wheel or a drag on the volume pill. Raising the volume ends a mute; lowering it does not.
        const float before = self.Volume();
        self.SetVolume(action.volume); if (self.Volume() > before) self.SetMuted(false);
        return true;
    }
    return false;
}
bool PlaybackCoordinator::SeekToSubtitle(const char* query, unsigned match_index) noexcept {
    return query && SeekToSubtitle(std::string_view{query}, match_index);
}
bool PlaybackCoordinator::SeekToSubtitle(std::string_view query, unsigned match_index) noexcept {
    subtitle_seek_ = {};
    const auto& search = *live_search_.load(std::memory_order_acquire); // API 17: the attached file's, once attached.
    if (!search.Ready()) return false;
    const auto begin = Now();
    subtitle::SearchMatch match{};
    subtitle_seek_.found = search.Find(query, match_index, match);
    if (!subtitle_seek_.found && search.Count(query) == 0 && &search == &search_) { // Concepts index the cues read at open.
        // In no cue: the cues that mean it (concept search), best first.
        ai::ConceptMatch meant{};
        if ((subtitle_seek_.found = subtitle_seek_.by_concept = concepts_.Find(query, match_index, meant))) {
            match = {meant.cue, meant.start, meant.end}; subtitle_seek_.confidence = meant.confidence;
        }
    }
    subtitle_seek_.lookup = Now() - begin;
    if (!subtitle_seek_.found) return false;
    subtitle_seek_.cue = match.cue; subtitle_seek_.start = match.start;
    return subtitle_seek_.sent = RequestSeek(match.start);
}
unsigned PlaybackCoordinator::SubtitleMatches(std::string_view query) const noexcept {
    const auto* search = live_search_.load(std::memory_order_acquire);
    const auto quoted = search->Count(query);
    return quoted || search != &search_ ? quoted : concepts_.Count(query); // Concepts: the cues read at open (not an attached file's).
}
ai::MediaBounds PlaybackCoordinator::Bounds() const noexcept {
    ai::MediaBounds bounds;
    // What a seek can land on: the video's extent (SendSeek clamps to it too).
    bounds.duration_100ns = video_.duration_100ns > 0 ? video_.duration_100ns : duration_;
    bounds.position_100ns = Position();
    bounds.volume = Volume(); bounds.muted = Muted(); bounds.paused = paused_;
    bounds.seekable = seekable_ && controls_ != nullptr; bounds.subtitles = live_search_.load(std::memory_order_acquire)->Ready() && Captions().Shown() != 0;
    bounds.chapters = timeline_.Chapters(); bounds.scenes = timeline_.Scenes();
    bounds.files_before = options_.files_before; bounds.files_after = options_.files_after;
    bounds.bookmarks = bookmarks_.Count();
    bounds.cure = conversation_.CurePending() && cures_.CanUndo();
    // The conversation's quote, counted in the subtitles shown now (a track change changes it).
    bounds.matches = bounds.subtitles && !conversation_.Query().empty() ? SubtitleMatches(conversation_.Query()) : 0;
    for (unsigned c = 0; c < track_count_; ++c) {
        if (tracks_[c].use != TrackUse::Selectable) continue;
        if (tracks_[c].kind == demux::TrackKind::Audio) ++bounds.audio_tracks;
        if (tracks_[c].kind == demux::TrackKind::Subtitle) ++bounds.subtitle_tracks;
    }
    return bounds;
}
std::int64_t PlaybackCoordinator::Position() const noexcept {
    // A seek still on its way is where playback already is, as far as the next command is concerned.
    return epoch_ > 1 && renderer_.LastSeek().epoch != epoch_ ? seek_target_ : renderer_.MediaPosition();
}
ai::Intent PlaybackCoordinator::Command(std::string_view utf8) noexcept { return dispatcher_.Dispatch(utf8, Bounds()); }
ai::Intent PlaybackCoordinator::Submit(std::string_view utf8) noexcept { return dispatcher_.Submit(utf8, Bounds()); }
bool PlaybackCoordinator::HandlePaletteSubmit(void* context, std::string_view text) noexcept {
    auto& self = *static_cast<PlaybackCoordinator*>(context);
    const auto asked = self.Now();
    auto intent = self.Submit(text);
    if (self.options_.typing && self.command_results_ < kMaxCommands) { // Scripted typing: recorded like a scripted command.
        auto& r = self.commands_[self.command_results_++]; r = {};
        intent.query = {}; // A view into the bar's text, which the next keystroke changes.
        r.intent = intent; r.resolve = self.Now() - asked; r.volume = self.Volume(); r.muted = self.Muted(); r.paused = self.paused_;
        r.by_concept = intent.kind == ai::IntentKind::FindSubtitle && self.subtitle_seek_.by_concept;
        { const auto shown = self.renderer_.Status().Text(); std::memcpy(r.status, shown.data(), (std::min)(shown.size(), sizeof(r.status) - 1)); }
        const bool seek = SeekingIntent(intent.kind);
        if (seek && intent.status == ai::IntentStatus::Ok) r.epoch = self.epoch_;
    }
    return intent.status == ai::IntentStatus::Ok;
}
ui::PaletteHint PlaybackCoordinator::HandlePalettePreview(void* context, std::string_view text, unsigned& matches) noexcept {
    // What Enter would do now: a command, or a search with this many cues to land on.
    const auto& self = *static_cast<const PlaybackCoordinator*>(context);
    const auto intent = ai::LocalIntentDispatcher::Interpret(text, self.Bounds());
    matches = 0;
    if (intent.status != ai::IntentStatus::Ok) return ui::PaletteHint::NoMatch;
    if (intent.kind != ai::IntentKind::FindSubtitle) return ui::PaletteHint::Command;
    matches = self.SubtitleMatches(intent.query);
    return matches ? ui::PaletteHint::Search : ui::PaletteHint::NoMatch;
}
ai::IntentStatus PlaybackCoordinator::HandleIntent(void* context, const ai::Intent& intent) noexcept {
    return static_cast<PlaybackCoordinator*>(context)->Execute(intent);
}
ai::IntentStatus PlaybackCoordinator::Execute(const ai::Intent& intent) noexcept {
    using ai::IntentKind; using ai::IntentStatus;
    const auto done = [](bool accepted) { return accepted ? IntentStatus::Ok : IntentStatus::Rejected; };
    switch (intent.kind) {
    case IntentKind::Play: return done(RequestPause(false));
    case IntentKind::Pause: return done(RequestPause(true));
    // Asking for more or for a level ends a mute; asking for less does not.
    case IntentKind::VolumeUp: case IntentKind::VolumeSet: SetVolume(intent.volume); SetMuted(false); return IntentStatus::Ok;
    case IntentKind::VolumeDown: SetVolume(intent.volume); return IntentStatus::Ok;
    case IntentKind::Mute: SetMuted(true); return IntentStatus::Ok;
    case IntentKind::Unmute: SetMuted(false); return IntentStatus::Ok;
    case IntentKind::SeekRelative: case IntentKind::SeekAbsolute: return done(RequestSeek(intent.target_100ns));
    case IntentKind::FindSubtitle: {
        if (!SeekToSubtitle(intent.query, intent.match)) return subtitle_seek_.found ? IntentStatus::Rejected : IntentStatus::NotFound;
        const bool meant = subtitle_seek_.by_concept;
        if (intent.from != IntentKind::None || meant) {
            // A follow-up: which match this is ("Match 2 of 5"); a concept match: the line it found, since the words
            // asked for are not in it (Concept match: "זהירות! שלושת הסנאים…"). Assembled in place.
            char text[ui::StatusCue::kMaxBytes]; unsigned n = 0;
            const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; };
            const auto number = [&](unsigned v) { char d[12]; unsigned k = 0; do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && k < 11);
                                                   while (k && n + 1 < sizeof(text)) text[n++] = d[--k]; };
            put(meant ? "Concept match" : "Match");
            if (intent.from != IntentKind::None) { put(" "); number(intent.match + 1); put(" of "); number(SubtitleMatches(intent.query)); }
            if (meant) { put(": "); n += static_cast<unsigned>(ui::QuoteExcerpt(Captions().Cues()[subtitle_seek_.cue].text, text + n, sizeof(text) - n, 28)); }
            text[n] = 0;
            Status(text, ui::StatusIcon::Subtitles, ui::StatusTone::Info);
        }
        return IntentStatus::Ok;
    }
    case IntentKind::ReplayCue: return ReplayCue();
    case IntentKind::CompareOn: case IntentKind::CompareOff: renderer_.SetComparison(intent.kind == IntentKind::CompareOn); return IntentStatus::Ok;
    // Follow-ups are resolved by the conversation before execution: they never arrive here.
    case IntentKind::NextMatch: case IntentKind::PreviousMatch: case IntentKind::NthMatch: case IntentKind::Undo: case IntentKind::Repeat: break;
    case IntentKind::SelectAudio: case IntentKind::SelectSubtitles: case IntentKind::SelectLanguage: return SelectTracks(intent);
    case IntentKind::NextChapter: return Navigate(true, 1);
    case IntentKind::PreviousChapter: return Navigate(true, -1);
    case IntentKind::NextScene: return Navigate(false, 1);
    case IntentKind::PreviousScene: return Navigate(false, -1);
    case IntentKind::SpeedUp: return StepSpeed(1) ? IntentStatus::Ok : IntentStatus::OutOfRange;
    case IntentKind::SpeedDown: return StepSpeed(-1) ? IntentStatus::Ok : IntentStatus::OutOfRange;
    case IntentKind::SpeedSet: return SetSpeed(intent.speed) ? IntentStatus::Ok : IntentStatus::Rejected;
    case IntentKind::ZoomIn: case IntentKind::ZoomOut: case IntentKind::ZoomReset:
        if (music_) { Status("Zoom \xc2\xb7 no picture to zoom", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f); return IntentStatus::Rejected; }
        if (intent.kind == IntentKind::ZoomReset) { renderer_.SetZoom(1.0f, 0.5f, 0.5f); return IntentStatus::Ok; }
        {
        const float zoom = renderer_.Zoom(), next = intent.kind == IntentKind::ZoomIn ? zoom * 2 : zoom / 2;
        if ((intent.kind == IntentKind::ZoomIn && zoom >= render::D3D12Renderer::kMaxZoom) || (intent.kind == IntentKind::ZoomOut && zoom <= 1.0f)) {
            Status(intent.kind == IntentKind::ZoomIn ? "Zoom \xc2\xb7 8\xc3\x97 is the closest" : "Zoom off", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
            return IntentStatus::OutOfRange;
        }
        renderer_.ZoomBy(next / zoom, 0.5f, 0.5f); // Into (or out of) the middle of what is shown.
        return IntentStatus::Ok;
        }
    case IntentKind::NextFile: return RequestFile(1);
    case IntentKind::PreviousFile: return RequestFile(-1);
    case IntentKind::LoopStart: return SetLoopStart();
    case IntentKind::LoopEnd: return SetLoopEnd();
    case IntentKind::LoopClear: return ClearLoop();
    case IntentKind::LoopCue: return LoopCue();
    case IntentKind::ShowStats: return ShowStats(true);
    case IntentKind::HideStats: return ShowStats(false);
    case IntentKind::ExplainQuality: return ShowStats(true, true);
    case IntentKind::NightOn: case IntentKind::NightOff: return SetNight(intent.kind == IntentKind::NightOn) ? IntentStatus::Ok : IntentStatus::Rejected;
    case IntentKind::BookmarkAdd: return AddBookmark();
    case IntentKind::BookmarkGo: return GoToBookmark(intent.match);
    case IntentKind::BookmarkNext: return NextBookmark();
    case IntentKind::BookmarkPrevious: return PreviousBookmark();
    case IntentKind::PinFrame: return PinFrame();
    case IntentKind::ComparePinned: return ComparePinned();
    case IntentKind::AudioZoomOn: case IntentKind::AudioZoomOff: return SetAudioZoom(intent.kind == IntentKind::AudioZoomOn) ? IntentStatus::Ok : IntentStatus::Rejected;
    case IntentKind::MicroMotionOn: case IntentKind::MicroMotionOff:
        return SetMicroMotion(intent.kind == IntentKind::MicroMotionOn) ? IntentStatus::Ok : IntentStatus::Rejected;
    case IntentKind::CureDark: case IntentKind::CureNoise: case IntentKind::CureMuffled: return Cure(intent.kind);
    case IntentKind::UndoCure: return UndoCure();
    case IntentKind::TrackTarget: case IntentKind::StopTracking: return Track(intent.kind == IntentKind::TrackTarget);
    case IntentKind::NextTarget: return NextTarget();
    case IntentKind::Snapshot: return Snapshot();
    case IntentKind::AudioSaccadeOn: case IntentKind::AudioSaccadeOff: return Saccade(intent.kind == IntentKind::AudioSaccadeOn);
    case IntentKind::RotateVideo: case IntentKind::RotateVideoBack: case IntentKind::LevelLeft: case IntentKind::LevelRight: case IntentKind::LevelHorizon:
        // docs/picture.md, "Rotation": a quarter turn, a degree of level, or straight again.
        if (music_) { Status("Rotation \xc2\xb7 no picture to turn", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f); return IntentStatus::Rejected; }
        if (intent.kind == IntentKind::RotateVideo || intent.kind == IntentKind::RotateVideoBack) renderer_.Rotate(intent.kind == IntentKind::RotateVideo ? 1 : -1);
        else if (intent.kind == IntentKind::LevelHorizon) { if (renderer_.LevelDegrees() == 0) return IntentStatus::NoContext; renderer_.Level(0, false); }
        else renderer_.Level(intent.kind == IntentKind::LevelRight ? 1.0f : -1.0f, true);
        return IntentStatus::Ok;
    case IntentKind::AutoLevel: case IntentKind::AutoLevelOff:
        // docs/picture.md, "Auto-leveling the horizon": on (already on: nothing to do), or off.
        if (music_) { Status("Auto level \xc2\xb7 no picture", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f); return IntentStatus::Rejected; }
        if (renderer_.AutoLevel() == (intent.kind == IntentKind::AutoLevel)) return IntentStatus::NoContext;
        renderer_.SetAutoLevel(intent.kind == IntentKind::AutoLevel);
        return IntentStatus::Ok;
    case IntentKind::Stabilize: case IntentKind::StabilizeOff:
        // docs/picture.md, "Stabilising the window": the view steadied from the next picture converted, or as it comes.
        if (music_) { Status("Stabilisation \xc2\xb7 no picture", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f); return IntentStatus::Rejected; }
        if (renderer_.Stabilization() == (intent.kind == IntentKind::Stabilize)) return IntentStatus::NoContext;
        if (!renderer_.SetStabilization(intent.kind == IntentKind::Stabilize)) {
            Status("Stabilisation \xc2\xb7 not on this picture", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f);
            return IntentStatus::Rejected;
        }
        return IntentStatus::Ok;
    case IntentKind::CropBars: case IntentKind::CropBarsOff:
        // docs/picture.md, "Black bars": the picture inside the bars encoded in it fills the view (as soon as they are known), or
        // the whole picture again.
        if (music_) { Status("Crop Â· no picture", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f); return IntentStatus::Rejected; }
        if (renderer_.AutoCrop() == (intent.kind == IntentKind::CropBars)) return IntentStatus::NoContext;
        renderer_.SetAutoCrop(intent.kind == IntentKind::CropBars);
        return IntentStatus::Ok;
    case IntentKind::None: break;
    }
    return IntentStatus::Rejected;
}
ai::IntentStatus PlaybackCoordinator::ReplayCue() noexcept {
    // The latest-starting cue shown at the position, or the last one that ended within kRecent of it (a line just said):
    // its start, with the cue on the landing picture.
    constexpr std::int64_t kRecent = 3 * ai::LocalIntentDispatcher::kSecond;
    const auto position = Position();
    const auto cues = Captions().Cues();
    std::int64_t start = -1;
    for (unsigned i = 0; i < cues.size(); ++i)
        if (Captions().Shown(i) && cues[i].start <= position && position < cues[i].end + kRecent && cues[i].start > start) start = cues[i].start;
    if (start < 0) { Status("No line to replay", ui::StatusIcon::Subtitles, ui::StatusTone::Warning); return ai::IntentStatus::NotFound; }
    return RequestSeek(start) ? ai::IntentStatus::Ok : ai::IntentStatus::Rejected;
}
ai::IntentStatus PlaybackCoordinator::SelectTracks(const ai::Intent& intent) noexcept {
    using ai::IntentKind; using ai::IntentStatus; using ai::TrackChoice; using demux::TrackKind;
    const auto language = [&](TrackKind kind, bool& refused) -> const MediaTrack* {
        // The language's selectable track (the file's default one first); `refused`: it exists but cannot play.
        const MediaTrack* found = nullptr; refused = false;
        for (unsigned c = 0; c < track_count_; ++c) {
            const auto& m = tracks_[c];
            if (m.kind != kind || std::strcmp(m.language, intent.language) != 0) continue;
            if (m.use != TrackUse::Selectable) { refused = true; continue; }
            if (!found || (m.enabled && !found->enabled)) found = &m;
        }
        return found;
    };
    const auto next = [&](TrackKind kind, std::uint32_t current) -> const MediaTrack* { // After the current one, round.
        int at = -1;
        for (unsigned c = 0; c < track_count_; ++c) if (tracks_[c].kind == kind && tracks_[c].id == current) at = static_cast<int>(c);
        for (unsigned k = 1; k <= track_count_; ++k) {
            const auto& m = tracks_[(static_cast<unsigned>(at + static_cast<int>(k)) + track_count_) % track_count_];
            if (m.kind == kind && m.use == TrackUse::Selectable && (m.id != current || kind == TrackKind::Subtitle)) return &m;
        }
        return nullptr;
    };
    const auto unavailable = [&](const char* what) {
        char text[96]; const auto* name = ai::LanguageName(intent.language);
        std::snprintf(text, sizeof(text), "%s %s cannot play here", name ? name : intent.language, what);
        Status(text, ui::StatusIcon::Warning, ui::StatusTone::Warning); return IntentStatus::Unavailable;
    };
    const auto missing = [&](const char* what) {
        char text[96]; const auto* name = ai::LanguageName(intent.language);
        std::snprintf(text, sizeof(text), "No %s %s", name ? name : intent.language, what);
        Status(text, ui::StatusIcon::Warning, ui::StatusTone::Warning); return IntentStatus::NotFound;
    };
    bool refused = false;
    if (intent.kind == IntentKind::SelectAudio || intent.kind == IntentKind::SelectLanguage) {
        if (intent.track == TrackChoice::Next) {
            const auto* m = next(TrackKind::Audio, audio_choice_);
            if (!m) { Status("No other audio track", ui::StatusIcon::Warning, ui::StatusTone::Warning); return IntentStatus::NotFound; }
            return SelectAudioTrack(m->id) ? IntentStatus::Ok : IntentStatus::Rejected;
        }
        if (intent.track == TrackChoice::Language) {
            if (const auto* m = language(TrackKind::Audio, refused)) return SelectAudioTrack(m->id) ? IntentStatus::Ok : IntentStatus::Rejected;
            const bool audio_refused = refused;
            if (intent.kind == IntentKind::SelectAudio) return audio_refused ? unavailable("audio") : missing("audio");
            // A language with no track named: its subtitles when there is no such audio.
            if (const auto* m = language(TrackKind::Subtitle, refused)) return SelectSubtitleTrack(m->id) ? IntentStatus::Ok : IntentStatus::Rejected;
            return audio_refused ? unavailable("audio") : refused ? unavailable("subtitles") : missing("audio or subtitles");
        }
        return IntentStatus::Rejected;
    }
    switch (intent.track) {
    case TrackChoice::Off: return SelectSubtitleTrack(kNoTrack) ? IntentStatus::Ok : IntentStatus::Rejected;
    case TrackChoice::On: {
        const auto* m = subtitle_last_ != kNoTrack ? FindTrack(subtitle_last_, TrackKind::Subtitle) : next(TrackKind::Subtitle, kNoTrack);
        return m && SelectSubtitleTrack(m->id) ? IntentStatus::Ok : IntentStatus::NotFound;
    }
    case TrackChoice::Next: {
        // Through every subtitle track, then none, then round again.
        const MediaTrack* m = nullptr;
        if (subtitle_choice_ == kNoTrack) m = next(TrackKind::Subtitle, kNoTrack);
        else {
            int at = -1, last = -1;
            for (unsigned c = 0; c < track_count_; ++c) if (tracks_[c].kind == TrackKind::Subtitle && tracks_[c].use == TrackUse::Selectable) {
                if (tracks_[c].id == subtitle_choice_) at = static_cast<int>(c);
                last = static_cast<int>(c);
            }
            if (at == last) return SelectSubtitleTrack(kNoTrack) ? IntentStatus::Ok : IntentStatus::Rejected;
            m = next(TrackKind::Subtitle, subtitle_choice_);
        }
        return m && SelectSubtitleTrack(m->id) ? IntentStatus::Ok : IntentStatus::NotFound;
    }
    case TrackChoice::Language:
        if (const auto* m = language(TrackKind::Subtitle, refused)) return SelectSubtitleTrack(m->id) ? IntentStatus::Ok : IntentStatus::Rejected;
        return refused ? unavailable("subtitles") : missing("subtitles");
    }
    return IntentStatus::Rejected;
}
void PlaybackCoordinator::TrackCue(const MediaTrack* m, bool audio) noexcept {
    // "Audio · Hebrew (עברית) · AAC stereo", "Subtitles · English", "Subtitles off".
    if (!m) { Status("Subtitles off", ui::StatusIcon::Subtitles, ui::StatusTone::Info); return; }
    char text[ui::StatusCue::kMaxBytes];
    (void)ui::TrackLabel(text, sizeof(text), audio, m->name, ai::LanguageName(m->language), ai::NativeLanguageName(m->language), m->language,
                         audio ? m->channels : 0, m->id);
    Status(text, audio ? ui::StatusIcon::Audio : ui::StatusIcon::Subtitles, ui::StatusTone::Info);
}
void PlaybackCoordinator::Status(const char* utf8, ui::StatusIcon icon, ui::StatusTone tone, float hold_seconds) noexcept {
    renderer_.ShowStatus(utf8 ? std::string_view{utf8} : std::string_view{}, icon, tone, hold_seconds);
}
namespace {
// Media time as m:ss.t (h:mm:ss.t past an hour), appended in place.
void PutClock(char* text, unsigned& n, unsigned size, std::int64_t ticks) noexcept {
    if (ticks < 0) ticks = 0;
    const std::int64_t tenths = (ticks + 500000) / 1000000, seconds = tenths / 10;
    const auto put = [&](std::int64_t v, unsigned width) {
        char d[20]; unsigned k = 0; do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && k < 19);
        while (k < width) d[k++] = '0';
        while (k && n + 1 < size) text[n++] = d[--k];
    };
    if (seconds >= 3600) { put(seconds / 3600, 1); if (n + 1 < size) text[n++] = ':'; put(seconds / 60 % 60, 2); }
    else put(seconds / 60, 1);
    if (n + 1 < size) text[n++] = ':';
    put(seconds % 60, 2);
    if (n + 1 < size) text[n++] = '.';
    put(tenths % 10, 1);
    text[n < size ? n : size - 1] = 0;
}
}
void PlaybackCoordinator::KeepAwake(bool playing) noexcept {
    // While playing, the system and the display stay on (docs/session.md, "Staying awake"): as any player, so that neither
    // the screen nor the machine goes to sleep under a film. Unattended, this machine otherwise entered standby every 40
    // minutes, and a picture's GPU work then waited ~20 minutes for its wake. Music mode needs the system, not the display.
    if (playing == awake_) return;
    awake_ = playing;
    // Released, the thread's own earlier request comes back (a host's, or a test process's StayAwake on this same thread): before
    // 2026-10-22 a pause cleared it.
    const auto before = SetThreadExecutionState(playing ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | (music_ ? 0u : ES_DISPLAY_REQUIRED)) :
                                                          static_cast<EXECUTION_STATE>(awake_prior_ | ES_CONTINUOUS));
    if (playing) awake_prior_ = before ? static_cast<std::uint32_t>(before) : static_cast<std::uint32_t>(ES_CONTINUOUS);
    if (!playing) {
        awake_held_ = static_cast<std::uint32_t>(before); // What was held until now, as Windows reports it.
        // The idle timers count on while the request is held: released after a long film, the machine would sleep at once.
        // Reset once, so the usual countdown starts from the end of playback (it did: a test started after hours of
        // unattended playing put the machine to sleep while preparing).
        (void)SetThreadExecutionState(ES_SYSTEM_REQUIRED | (music_ ? 0u : ES_DISPLAY_REQUIRED));
    }
}
void PlaybackCoordinator::WakeWorker() noexcept {
    // A command or a returned picture for the worker, if it waits (the same handshake as WakePresenter's).
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (worker_waiting_.load(std::memory_order_relaxed)) (void)SetEvent(worker_wake_);
}
bool PlaybackCoordinator::CommitControl() noexcept {
    if (!controls_->CommitWriteSlot()) return false;
    WakeWorker(); return true;
}
void PlaybackCoordinator::WakePresenter() noexcept {
    // Worker: a picture committed (or the end of input) while the presentation thread waits. The fence pairs with Pace's: either
    // this sees it waiting, or it sees the picture before it waits.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (pace_waiting_.load(std::memory_order_relaxed)) (void)SetEvent(pace_picture_);
}
void PlaybackCoordinator::Pace() noexcept {
    // Low-power pacing (docs/session.md, "Low-power pacing"): the end of a presentation pass. The thread spins (as it always
    // did) while something is in motion that a wait would make late: the start before the clock runs, a seek until its
    // picture is on screen, frame steps, a device recovery, a resize, a picture the clock gives no deadline yet, a decoded
    // picture that can be converted now. Otherwise it waits for the earliest of: the next picture's deadline less kPaceSpin
    // (then spins the rest), the controls' next refresh or advance, a window message, the worker's next picture, and the
    // GPU fence the next conversion waits on.
    const auto spin = [this](Pacing::Spin why) { ++pacing_.spins[static_cast<unsigned>(why)]; YieldProcessor(); };
    if (!pace_timer_) return spin(Pacing::Spin::Off);
    const auto now = Now();
    if (error_.load(std::memory_order_relaxed) || renderer_.LastSeek().epoch != epoch_ || (!paused_ && !clock_started_.load(std::memory_order_relaxed)) ||
        recovering_ || gpu_phase_.load(std::memory_order_relaxed) != kGpuRunning || pending_steps_ || step_epoch_ || scrub_pending_ || adopt_wait_ ||
        renderer_.ResizePending() || file_request_ != FileRequest::None || stop_requested_.load(std::memory_order_relaxed)) return spin(Pacing::Spin::Moving);
    auto wake = renderer_.NextWake();
    if (renderer_.Settled() && script_wake_ < wake.at) wake.at = (std::max)(script_wake_, now); // A script due first.
    HANDLE handles[4]{pace_timer_, pace_picture_, nullptr, nullptr}; DWORD count = 2;
    if (const HANDLE input = renderer_.MessageThread() ? renderer_.InputWake() : nullptr) handles[count++] = input; // Input wakes it at once.
    const auto* frame = frames_->PeekReadSlot();
    if (frame) {
        if (static_cast<unsigned>(frame->flags) & static_cast<unsigned>(core::VideoFrameFlags::NoDisplay)) return spin(Pacing::Spin::Convert);
        const auto block = renderer_.ConvertWait(*frame, decoder_);
        if (block == render::D3D12Renderer::ConvertBlock::Now) return spin(Pacing::Spin::Convert);
        if (block != render::D3D12Renderer::ConvertBlock::Presentation) {
            if (!renderer_.ArmConvertWake(block, *frame, decoder_, pace_fence_)) return spin(Pacing::Spin::Convert);
            handles[count++] = pace_fence_;
        }
    }
    if (loop_.Looping() && !paused_) {
        // The loop's B is judged by the clock each pass (PumpLoop): a deadline like a picture's.
        clock::ClockEstimate e{};
        if (clock_.Estimate(now, e) == clock::ClockQuery::Ready && e.epoch == epoch_ && e.speed > 0) {
            const auto b = now + static_cast<std::int64_t>(static_cast<double>(loop_.B() - e.media_time_100ns) / e.speed);
            if (b < wake.at) wake = {b, true, true};
        }
    }
    const auto target = wake.precise ? wake.at - pace_spin_ : wake.at;
    if (wake.at <= now) return spin(Pacing::Spin::Present);
    if (wake.precise ? wake.at - now <= pace_floor_ : target - now <= kPaceSpin / 2) return spin(wake.precise ? Pacing::Spin::Deadline : Pacing::Spin::Controls);
    LARGE_INTEGER due{}; due.QuadPart = -(target - now); // Relative, in 100 ns.
    if (!SetWaitableTimerEx(pace_timer_, &due, 0, nullptr, nullptr, nullptr, 0)) return spin(Pacing::Spin::Off);
    pace_waiting_.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    // A picture committed after the look above (without seeing the flag) is not waited past.
    if (!frame && frames_->PeekReadSlot()) { pace_waiting_.store(false, std::memory_order_relaxed); return; }
    if (host_commands_.PeekReadSlot()) { pace_waiting_.store(false, std::memory_order_relaxed); return; } // A host's command (API 12).
    // While a picture has a deadline the thread waits on its kernel objects alone: a message wait's return is late now and then
    // (1 % of returns over 1 ms, some 4-38 ms, against at most 1.8 ms for the same timer alone), and woken late before a
    // picture it made the picture late (dropped once, across a pause). Such a wait lasts at most one advance of the controls
    // (8.3 ms), so a window message waits no longer than that; without a picture due (paused, the controls alone) the thread
    // wakes for messages too.
    // With a window thread (docs/session.md, "The window thread") input comes as the ring's event: kernel objects only, always.
    const auto result = wake.picture || renderer_.MessageThread() ? WaitForMultipleObjects(count, handles, FALSE, INFINITE) :
                                       MsgWaitForMultipleObjectsEx(count, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    pace_waiting_.store(false, std::memory_order_relaxed);
    const auto woke = Now();
    auto& p = pacing_;
    ++p.waits; p.waited += woke - now;
    if (paused_) { ++p.paused_waits; if (renderer_.Settled()) ++p.settled_waits; }
    if (result != WAIT_OBJECT_0) ++p.early;
    else if (wake.precise) { // How late for a picture.
        ++p.timed; p.late_sum += woke - target; p.late_max = (std::max)(p.late_max, woke - target);
        if (woke - target > 10000) ++p.late_1ms;
        if (woke - target > 40000) ++p.late_4ms;
        if (woke > wake.at) { ++p.past_deadline; p.past_sum += woke - wake.at; p.past_max = (std::max)(p.past_max, woke - wake.at); }
    }
}
void PlaybackCoordinator::AccountCpu() noexcept {
    const auto cpu = [](BOOL ok, const FILETIME& kernel, const FILETIME& user) {
        if (!ok) return std::int64_t{0};
        return static_cast<std::int64_t>((static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                                         (static_cast<std::uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime));
    };
    FILETIME created{}, exited{}, kernel{}, user{};
    const auto presentation = cpu(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user), kernel, user);
    const auto worker = worker_ ? cpu(GetThreadTimes(worker_, &created, &exited, &kernel, &user), kernel, user) : 0;
    const auto process = cpu(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), kernel, user);
    const auto wall = Now();
    if (cpu_wall_) {
        auto& s = paused_ ? pacing_.paused : pacing_.playing;
        s.wall += wall - cpu_wall_; s.presentation += presentation - cpu_presentation_; s.worker += worker - cpu_worker_; s.process += process - cpu_process_;
    }
    cpu_wall_ = wall; cpu_presentation_ = presentation; cpu_worker_ = worker; cpu_process_ = process;
}
const char* PlaybackCoordinator::Name(Phase phase) noexcept {
    static constexpr const char* names[]{"idle", "preparing", "window messages", "stream", "scripts", "stopping the worker",
                                         "closing", "worker: waiting for Run", "worker: loop", "worker: commands", "worker: track switch",
                                         "worker: seek", "worker: media", "worker: stopped", "worker: teardown", "joined",
                                         "stream: converting a picture", "stream: presenting"};
    const auto i = static_cast<unsigned>(phase);
    return i < std::size(names) ? names[i] : "?";
}
std::int64_t PlaybackCoordinator::PrecisePosition() const noexcept {
    // While playing, the clock itself (the HUD's position is sampled every 83 ms).
    clock::ClockEstimate e{};
    if (!paused_ && renderer_.LastSeek().epoch == epoch_ && clock_.Estimate(Now(), e) == clock::ClockQuery::Ready && e.epoch == epoch_)
        return e.media_time_100ns;
    return Position();
}
void PlaybackCoordinator::LoopChanged() noexcept {
    const auto duration = Bounds().duration_100ns;
    renderer_.SetLoop(loop_.Packed(duration));
    loop_epoch_ = 0;
    // The pinned turnaround: the renderer pins the pictures from A, the worker the PCM, on the next pass through A.
    if (loop_.Looping() && !music_) renderer_.ArmLoopPins(loop_.A(), loop_.B()); else renderer_.ClearLoopPins();
    if (controls_) {
        if (auto* command = controls_->AcquireWriteSlot()) {
            *command = {}; command->kind = ControlCommand::Kind::LoopAnchor; command->target_100ns = loop_.Looping() && !music_ ? loop_.A() : INT64_MIN;
            (void)CommitControl();
        }
    }
    const auto now = Now();
    if (loop_.Looping()) ui_events_.Record(now, core::FlightEvent::LoopSet, loop_.FromCue() ? 1 : 0, 0, loop_.A(), loop_.B());
    else if (loop_.Current() == timeline::LoopRegion::State::Off) ui_events_.Record(now, core::FlightEvent::LoopCleared);
}
void PlaybackCoordinator::LoopCueText(timeline::LoopRegion::Step step) noexcept {
    // "Loop · A at 0:12.4 · L again for B", "Loop · 0:12.4 – 0:15.8", "Loop · sentence 0:12.3 – 0:15.9", "Loop off".
    using Step = timeline::LoopRegion::Step;
    char text[96]; unsigned n = 0;
    const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; text[n] = 0; };
    auto tone = ui::StatusTone::Info;
    switch (step) {
    case Step::MarkedStart: put("Loop \xc2\xb7 A at "); PutClock(text, n, sizeof(text), loop_.A()); put(" \xc2\xb7 L again for B"); break;
    case Step::Looping:
        put(loop_.FromCue() ? "Loop \xc2\xb7 sentence " : "Loop \xc2\xb7 ");
        PutClock(text, n, sizeof(text), loop_.A()); put(" \xe2\x80\x93 "); PutClock(text, n, sizeof(text), loop_.B());
        break;
    case Step::Cleared: put("Loop off"); break;
    case Step::TooShort: put("Loop \xc2\xb7 too short (under 0.2 s)"); tone = ui::StatusTone::Warning; break;
    case Step::NoStart: put("Loop \xc2\xb7 set its start first"); tone = ui::StatusTone::Warning; break;
    case Step::Unavailable: put("Loop \xc2\xb7 this media cannot loop"); tone = ui::StatusTone::Warning; break;
    }
    Status(text, tone == ui::StatusTone::Warning ? ui::StatusIcon::Warning : ui::StatusIcon::None, tone, 2.0f);
}
namespace {
ai::IntentStatus LoopOutcome(timeline::LoopRegion::Step step) noexcept {
    using Step = timeline::LoopRegion::Step;
    switch (step) {
    case Step::MarkedStart: case Step::Looping: case Step::Cleared: return ai::IntentStatus::Ok;
    case Step::TooShort: return ai::IntentStatus::OutOfRange;
    case Step::NoStart: return ai::IntentStatus::NoContext;
    case Step::Unavailable: break;
    }
    return ai::IntentStatus::Unavailable;
}
}
ai::IntentStatus PlaybackCoordinator::ToggleLoop() noexcept {
    const auto step = seekable_ && controls_ ? loop_.Toggle(PrecisePosition(), Bounds().duration_100ns) : timeline::LoopRegion::Step::Unavailable;
    LoopCueText(step);
    const auto outcome = LoopOutcome(step);
    if (outcome == ai::IntentStatus::Ok) LoopChanged();
    return outcome;
}
ai::IntentStatus PlaybackCoordinator::SetLoopStart() noexcept {
    const auto step = seekable_ && controls_ ? loop_.SetStart(PrecisePosition(), Bounds().duration_100ns) : timeline::LoopRegion::Step::Unavailable;
    LoopCueText(step);
    const auto outcome = LoopOutcome(step);
    if (outcome == ai::IntentStatus::Ok) LoopChanged();
    return outcome;
}
ai::IntentStatus PlaybackCoordinator::SetLoopEnd() noexcept {
    const auto step = seekable_ && controls_ ? loop_.SetEnd(PrecisePosition(), Bounds().duration_100ns) : timeline::LoopRegion::Step::Unavailable;
    LoopCueText(step);
    const auto outcome = LoopOutcome(step);
    if (outcome == ai::IntentStatus::Ok) LoopChanged();
    return outcome;
}
ai::IntentStatus PlaybackCoordinator::ClearLoop() noexcept {
    LoopCueText(loop_.Clear()); LoopChanged();
    return ai::IntentStatus::Ok;
}
ai::IntentStatus PlaybackCoordinator::LoopCue() noexcept {
    // The cue ReplayCue would replay: the latest-starting one shown at the position, or the last one that ended within 3 s
    // of it (a line just said). Its interval, padded, is the loop; playback goes to its start at once.
    constexpr std::int64_t kRecent = 3 * ai::LocalIntentDispatcher::kSecond;
    const auto position = PrecisePosition();
    const auto cues = Captions().Cues();
    int found = -1;
    for (unsigned i = 0; i < cues.size(); ++i)
        if (Captions().Shown(i) && cues[i].start <= position && position < cues[i].end + kRecent && (found < 0 || cues[i].start > cues[static_cast<unsigned>(found)].start))
            found = static_cast<int>(i);
    if (found < 0) { Status("Loop \xc2\xb7 no line on screen to loop", ui::StatusIcon::Subtitles, ui::StatusTone::Warning); return ai::IntentStatus::NotFound; }
    const auto& cue = cues[static_cast<unsigned>(found)];
    const auto step = loop_.SetCue(cue.start, cue.end, Bounds().duration_100ns);
    LoopCueText(step);
    const auto outcome = LoopOutcome(step);
    if (outcome != ai::IntentStatus::Ok) return outcome;
    loop_run_.cue_start = cue.start; loop_run_.cue_end = cue.end; loop_run_.cue_index = static_cast<unsigned>(found);
    LoopChanged();
    if (!SendSeek(loop_.A())) return ai::IntentStatus::Rejected;
    conversation_.NoteJump(position);
    return ai::IntentStatus::Ok;
}
void PlaybackCoordinator::PumpLoop(bool finished) noexcept {
    // The newest turn's sound: where it started and when the clock ran again.
    if (loop_run_.count && loop_run_.count <= kMaxTurns) {
        auto& t = loop_run_.turns[loop_run_.count - 1];
        const auto anchored = anchored_at_.load(std::memory_order_acquire);
        if (t.landed && !t.resumed && anchored > t.requested) { t.resumed = anchored - t.requested; t.audio = audio_start_.load(std::memory_order_acquire); }
    }
    // The newest turn on screen: its landing.
    if (loop_epoch_ && renderer_.LastSeek().epoch == loop_epoch_) {
        if (loop_run_.count && loop_run_.count <= kMaxTurns) {
            auto& t = loop_run_.turns[loop_run_.count - 1]; const auto& landed = renderer_.LastSeek();
            if (!t.landed && t.epoch == loop_epoch_) { t.landed = true; t.latency = landed.latency; t.pts = landed.pts; t.duration = landed.duration; t.waited = landed.waited; }
        }
        loop_epoch_ = 0;
    }
    if (!loop_.Looping() || paused_ || !seekable_ || loop_epoch_ || epoch_ == UINT32_MAX) return;
    if (renderer_.LastSeek().epoch != epoch_) return; // Another seek is landing: B is judged on its timeline.
    // B is judged by the clock itself, every pass of the UI loop.
    clock::ClockEstimate e{};
    const auto now = Now();
    const bool timed = clock_.Estimate(now, e) == clock::ClockQuery::Ready && e.epoch == epoch_;
    // The pinned turn's landing staged as B nears (2026-11-02): its copy runs on the GPU before the turn, which then waits for
    // nothing (a landing's copy queued behind the GPU's other work cost 5-7 ms about once a full suite).
    if (timed && speed_ == 1.0f && !music_ && renderer_.LoopPinsReady() && e.media_time_100ns + kStageAhead >= loop_.B()) (void)renderer_.StageLoopLanding();
    if (!(timed && loop_.Due(e.media_time_100ns)) && !finished) return;
    const auto seen = timed ? e.media_time_100ns : loop_.B();
    // Through the pinned pictures when both halves are pinned and the PCM covers them (docs/seeking.md, "Pinned loop
    // turnaround"); else an ordinary seek to A.
    const auto end = renderer_.LoopPinEnd();
    const bool pinned = renderer_.LoopPinsReady() && speed_ == 1.0f && !music_ && end > loop_.A() &&
                        loop_pcm_anchor_.load(std::memory_order_acquire) == loop_.A() && loop_pcm_end_.load(std::memory_order_relaxed) >= end &&
                        SendPinnedSeek(loop_.A(), end);
    if (!pinned && !SendSeek(loop_.A())) return;
    loop_.NoteWrap(); loop_epoch_ = epoch_;
    if (loop_run_.count < kMaxTurns) loop_run_.turns[loop_run_.count] = {loop_.A(), loop_.B(), seen, now, 0, 0, 0, epoch_, false, loop_.FromCue(), pinned};
    ++loop_run_.count;
    ui_events_.Record(now, core::FlightEvent::LoopWrap, pinned ? 1 : 0, loop_.Wraps(), loop_.B(), loop_.A());
}
ui::TelemetrySnapshot PlaybackCoordinator::Telemetry() noexcept {
    ui::TelemetrySnapshot s;
    s.now = Now();
    const auto& g = renderer_.Governor();
    s.mode = g.Mode(); s.tier = g.Tier(); s.timing = renderer_.Timing(); s.neural_available = renderer_.NeuralAvailable();
    s.p95 = g.P95(); s.budget = g.Budget(); s.samples = g.Window().Count(); s.changes = g.Changes(); s.last_change = g.LastChange();
    const render::GovernorOptions thresholds{}; // The renderer's governor runs with the defaults.
    s.step_down = static_cast<float>(thresholds.step_down); s.step_up = static_cast<float>(thresholds.step_up);
    s.picture = !music_ && video_.id != 0;
    s.codec = hevc_ ? "HEVC" : "H.264"; s.bits = ten_bit_ ? 10u : 8u;
    s.transfer = picture_.color.transfer == 16 ? "PQ" : picture_.color.transfer == 18 ? "HLG" : "";
    s.source_width = picture_.right - picture_.left; s.source_height = picture_.bottom - picture_.top;
    const auto& v = renderer_.LastVideoRect(); s.shown_width = v.width; s.shown_height = v.height; s.zoom = renderer_.Zoom();
    s.audio = routing_.status == AudioStatus::Ready; s.audio_unsupported = routing_.status == AudioStatus::UnsupportedLayout;
    s.sample_rate = audio_.sample_rate; s.source_channels = routing_.source_channels; s.output_channels = routing_.output_channels;
    s.fold_down = routing_.fold_down; s.binaural = SpatialAudio(); s.volume = Volume(); s.muted = Muted(); s.speed = speed_;
    { const auto l = LoudnessNow(); s.loudness = s.audio; s.night = Night(); s.lufs_short = l.short_term; s.true_peak = l.true_peak; s.night_gain = l.gain; }
    s.presented = renderer_.Presented(); s.dropped = renderer_.Dropped(); s.drop_waited = renderer_.LastDrop().waited;
    const auto tiles = renderer_.NeuralTiles(); s.tiles_reused = tiles.reused; s.tiles_evaluated = tiles.evaluated;
    s.loop = loop_.Looping(); s.loop_a = loop_.A(); s.loop_b = loop_.B(); s.loop_wraps = loop_.Wraps();
    s.event_count = core::MergeNewest(ui_events_, worker_events_, merged_, kTelemetryEvents, scratch_a_, scratch_b_); s.events = merged_;
    return s;
}
ai::IntentStatus PlaybackCoordinator::ShowStats(bool show, bool explain) noexcept {
    ui_events_.Record(Now(), core::FlightEvent::Stats, explain ? 1 : 0);
    if (!show) {
        // Hidden: the panel, if it is still the cue on screen.
        if (stats_shown_ && renderer_.Status().Revision() == stats_revision_) renderer_.ShowStatus({}, ui::StatusIcon::None, ui::StatusTone::Info);
        stats_shown_ = false;
        return ai::IntentStatus::Ok;
    }
    const auto snapshot = Telemetry();
    char text[ui::StatusCue::kMaxBytes];
    if (!(explain ? ui::ExplainQuality(snapshot, text, sizeof(text)) : ui::ComposeStats(snapshot, text, sizeof(text)))) return ai::IntentStatus::Rejected;
    Status(text, ui::StatusIcon::Device, ui::StatusTone::Info, explain ? 10.0f : ui::StatusCue::kMaxHold);
    stats_revision_ = renderer_.Status().Revision(); stats_shown_ = !explain; stats_refresh_ = stats_shown_at_ = Now();
    if (options_.test_telemetry && telemetry_run_.count < kMaxReadouts) {
        auto& r = telemetry_run_.readouts[telemetry_run_.count++];
        r.snapshot = snapshot; r.snapshot.events = nullptr; r.explain = explain;
        const auto shown = renderer_.Status().Text(); std::memcpy(r.text, shown.data(), (std::min)(shown.size(), sizeof(r.text) - 1));
    }
    return ai::IntentStatus::Ok;
}
void PlaybackCoordinator::PumpStats() noexcept {
    // While the panel is up it follows the engine, once a second; another cue over it ends that.
    if (!stats_shown_) return;
    if (renderer_.Status().Revision() != stats_revision_) { stats_shown_ = false; return; }
    const auto now = Now();
    if (now - stats_refresh_ < 10000000) return;
    stats_refresh_ = now;
    const auto snapshot = Telemetry();
    char text[ui::StatusCue::kMaxBytes];
    const auto n = ui::ComposeStats(snapshot, text, sizeof(text));
    const auto shown = renderer_.Status().Text();
    // An unchanged text is shown again only to keep the panel up (a cue's hold is at most 30 s).
    if (!n || (shown == std::string_view{text, n} && now - stats_shown_at_ < 200000000)) return;
    Status(text, ui::StatusIcon::Device, ui::StatusTone::Info, ui::StatusCue::kMaxHold);
    stats_revision_ = renderer_.Status().Revision(); stats_shown_at_ = now;
}
void PlaybackCoordinator::NoteEvents() noexcept {
    // What the renderer did since the last look: landings, quality tier changes, drops, zoom.
    if (const auto& landed = renderer_.LastSeek(); landed.epoch != noted_landing_) {
        noted_landing_ = landed.epoch;
        if (landed.epoch > 1) ui_events_.Record(landed.presented, core::FlightEvent::SeekLanded, 0, landed.epoch, landed.pts, landed.latency);
    }
    const auto& g = renderer_.Governor();
    if (g.Changes() != noted_changes_) {
        noted_changes_ = g.Changes(); const auto& c = g.LastChange();
        ui_events_.Record(c.at, core::FlightEvent::TierChange, static_cast<std::uint16_t>(static_cast<unsigned>(c.from) << 8 | static_cast<unsigned>(c.to)),
                          static_cast<std::uint32_t>(c.reason), c.p95, c.budget);
    }
    if (renderer_.Dropped() != noted_drops_) {
        noted_drops_ = renderer_.Dropped(); const auto& d = renderer_.LastDrop();
        ui_events_.Record(Now(), core::FlightEvent::Drop, d.waited, d.behind_clock ? 1u : 0u, d.pts, d.lateness);
    }
    if (renderer_.Zoom() != noted_zoom_ || renderer_.ZoomX() != noted_zoom_x_ || renderer_.ZoomY() != noted_zoom_y_) {
        noted_zoom_ = renderer_.Zoom(); noted_zoom_x_ = renderer_.ZoomX(); noted_zoom_y_ = renderer_.ZoomY();
        ui_events_.Record(Now(), core::FlightEvent::Zoom, 0, static_cast<std::uint32_t>(std::lround(noted_zoom_ * 100)),
                          std::lround(noted_zoom_x_ * 1e6), std::lround(noted_zoom_y_ * 1e6));
    }
}
bool PlaybackCoordinator::SendPinnedSeek(std::int64_t a, std::int64_t end) noexcept {
    // As SendSeek, but the renderer queues the pinned pictures from A at once, and the worker decodes from where they end.
    if (epoch_ == UINT32_MAX || !controls_) return false;
    auto* command = controls_->AcquireWriteSlot();
    if (!command) return false;
    sent_at_ = Now();
    if (!renderer_.BeginPinnedEpoch(epoch_ + 1, sent_at_, a, end)) { (void)controls_->CancelWriteSlot(); return false; }
    *command = {}; command->kind = ControlCommand::Kind::Seek; command->epoch = epoch_ + 1; command->target_100ns = end;
    command->reserved[1] = 1;
    command->reserved[2] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(a)); command->reserved[3] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(a) >> 32);
    if (!CommitControl()) return false;
    ++epoch_; ++seeks_; in_flight_ = true; seek_target_ = a; adopt_wait_ = 0;
    ui_events_.Record(sent_at_, core::FlightEvent::SeekRequested, 0, epoch_, a);
    return true;
}
bool PlaybackCoordinator::PrepareModules() noexcept {
    // Each enabled module's region, first in the arena and in module order (docs/sdk.md, "Modules"): a module left out moves
    // what follows by its whole region (a multiple of 64 KiB), so no alignment padding changes with the module set.
    for (unsigned i = 0; i < kModuleCount; ++i) {
        module_used_[i] = false;
        const auto bytes = static_cast<std::size_t>(kModuleBudgets[i].arena_bytes);
        if (!(options_.modules & (1u << i)) || !bytes) continue;
        void* region = arena_.Allocate(bytes, 64);
        if (!region || !module_arenas_[i].InitializeView(region, bytes)) return false;
        module_used_[i] = true;
    }
    return true;
}
unsigned PlaybackCoordinator::Threads() const noexcept {
    return (worker_ ? 1u : 0u) + (voice_thread_ ? 1u : 0u) + (snapshot_thread_ ? 1u : 0u) + renderer_.TextThreads();
}
// ---- Embedding (API 12; docs/sdk.md, "Embedding: API 12") ----
bool PlaybackCoordinator::PostCommand(std::string_view utf8, std::uint32_t id) noexcept {
    if (utf8.size() >= sizeof(HostCommand::text)) return false;
    auto* c = host_commands_.AcquireWriteSlot();
    if (!c) return false;
    std::memcpy(c->text, utf8.data(), utf8.size()); c->text[utf8.size()] = 0;
    c->length = static_cast<std::uint32_t>(utf8.size()); c->id = id; c->op = 0; c->value = 0;
    if (!host_commands_.CommitWriteSlot()) return false;
    WakePresenter(); // Paused and settled too: the command runs now, not at the next refresh.
    return true;
}
bool PlaybackCoordinator::PostControl(HostControl op, double value, std::uint32_t id) noexcept {
    auto* c = host_commands_.AcquireWriteSlot();
    if (!c) return false;
    c->text[0] = 0; c->length = 0; c->id = id; c->op = static_cast<std::uint32_t>(op); c->value = value;
    if (!host_commands_.CommitWriteSlot()) return false;
    WakePresenter();
    return true;
}
ai::IntentStatus PlaybackCoordinator::Control(HostControl op, double value) noexcept {
    if (op == HostControl::kStepFrame) return StepFrame(value > 0 ? 1 : -1) ? ai::IntentStatus::Ok : ai::IntentStatus::Unavailable;
    if (op == HostControl::kAudioTrack) return SelectAudioTrack(static_cast<std::uint32_t>(value)) ? ai::IntentStatus::Ok : ai::IntentStatus::Unavailable;
    if (op == HostControl::kSubtitleTrack)
        return SelectSubtitleTrack(value < 0 ? kNoTrack : static_cast<std::uint32_t>(value)) ? ai::IntentStatus::Ok : ai::IntentStatus::Unavailable;
    if ((op == HostControl::kSeek || op == HostControl::kSeekRelative) && !parser_.Seekable()) return ai::IntentStatus::Unavailable;
    ai::Intent intent{};
    if (!ControlIntent(op, value, PrecisePosition(), duration_, intent)) return ai::IntentStatus::Malformed;
    return Execute(intent);
}
void PlaybackCoordinator::DrainHostCommands() noexcept {
    for (unsigned i = 0; i != 8; ++i) {
        const auto* c = host_commands_.PeekReadSlot();
        if (!c) return;
        // API 15: a structured control runs as the intent its command would have become; a text goes through the grammar.
        const auto status = c->op ? Control(static_cast<HostControl>(c->op), c->value) : Command(std::string_view{c->text, c->length}).status;
        HostEvent e{}; e.kind = HostEvent::kCommand; e.time = Now(); e.id = c->id; e.status = status == ai::IntentStatus::Ok ? 0 : 1;
        (void)host_commands_.ReleaseReadSlot();
        Emit(e);
    }
}
void PlaybackCoordinator::Emit(const HostEvent& event) noexcept {
    auto* e = host_events_.AcquireWriteSlot();
    if (!e) { ++events_lost_; return; } // The host has not polled: counted on the next event that fits.
    *e = event; e->lost = events_lost_; events_lost_ = 0;
    (void)host_events_.CommitWriteSlot();
}
bool PlaybackCoordinator::PollEvent(HostEvent& out) noexcept {
    const auto* e = host_events_.PeekReadSlot();
    if (!e) return false;
    out = *e;
    return host_events_.ReleaseReadSlot();
}
EmbedState PlaybackCoordinator::PublishedState() const noexcept {
    EmbedState s;
    s.position = published_position_.load(std::memory_order_relaxed); s.duration = published_duration_.load(std::memory_order_relaxed);
    const auto flags = published_flags_.load(std::memory_order_relaxed);
    s.paused = (flags & 1u) != 0; s.muted = (flags & 2u) != 0;
    s.volume = published_volume_.load(std::memory_order_relaxed); s.speed = published_speed_.load(std::memory_order_relaxed);
    s.presented = published_presented_.load(std::memory_order_relaxed); s.dropped = published_dropped_.load(std::memory_order_relaxed);
    return s;
}
void PlaybackCoordinator::PublishHost() noexcept {
    const auto now = Now();
    const auto position = PrecisePosition();
    const bool muted = Muted();
    const float volume = Volume(), speed = Speed();
    published_position_.store(position, std::memory_order_relaxed); published_duration_.store(duration_, std::memory_order_relaxed);
    published_flags_.store((paused_ ? 1u : 0u) | (muted ? 2u : 0u), std::memory_order_relaxed);
    published_volume_.store(volume, std::memory_order_relaxed); published_speed_.store(speed, std::memory_order_relaxed);
    published_presented_.store(renderer_.Presented(), std::memory_order_relaxed); published_dropped_.store(renderer_.Dropped(), std::memory_order_relaxed);
    if (attach_choice_.exchange(false, std::memory_order_acq_rel)) subtitle_choice_ = subtitle_last_ = 0; // API 17: an attached file is shown.
    published_audio_track_.store(audio_choice_, std::memory_order_relaxed); published_subtitle_track_.store(subtitle_choice_, std::memory_order_relaxed);
    const auto event = [&](std::uint32_t kind) {
        HostEvent e{}; e.kind = kind; e.time = now; e.position = position; e.duration = duration_;
        e.paused = paused_ ? 1 : 0; e.muted = muted ? 1 : 0; e.volume = volume; e.speed = speed;
        return e;
    };
    if (!host_state_known_ || paused_ != last_paused_ || muted != last_muted_ || volume != last_volume_ || speed != last_speed_) {
        const bool pausing = host_state_known_ && paused_ && !last_paused_;
        host_state_known_ = true; last_paused_ = paused_; last_muted_ = muted; last_volume_ = volume; last_speed_ = speed;
        Emit(event(HostEvent::kState));
        if (pausing) { Emit(event(HostEvent::kPosition)); last_position_event_ = now; } // Where it stopped.
    }
    // Ten a second (API 15; four before): a host's time label and slider move smoothly. The ring holds 256 events: 25 s unpolled.
    if (!paused_ && clock_started_.load(std::memory_order_relaxed) && now - last_position_event_ >= 1000000) { Emit(event(HostEvent::kPosition)); last_position_event_ = now; }
    // A seek's first picture on screen (the start's own first picture is not a seek: it is only noted).
    if (const auto& seek = renderer_.LastSeek(); seek.epoch && seek.epoch != last_seek_epoch_) {
        if (last_seek_epoch_ && seek.epoch == epoch_) {
            auto e = event(HostEvent::kSeekLanded); e.position = seek.pts; e.latency = seek.latency;
            Emit(e);
        }
        last_seek_epoch_ = seek.epoch;
    }
    if (const auto asked = renderer_.FullscreenRequests(); asked != fullscreen_seen_) { fullscreen_seen_ = asked; Emit(event(HostEvent::kFullscreen)); }
}
bool PlaybackCoordinator::PollFrame(FrameSignature& out) noexcept {
    const auto* slot = signatures_ ? signatures_->PeekReadSlot() : nullptr;
    if (!slot) return false;
    out = *slot; (void)signatures_->ReleaseReadSlot();
    return true;
}
void PlaybackCoordinator::OnSignature(void* context, const render::D3D12Renderer::PictureSignature& p) noexcept {
    // UI thread, in the presentation pass: the picture's statistics with what the DSP's analysis hears now.
    auto& self = *static_cast<PlaybackCoordinator*>(context);
    FrameSignature s{};
    s.pts_100ns = p.pts_100ns; s.presented_100ns = p.presented; s.sequence = self.signature_count_.load(std::memory_order_relaxed);
    s.epoch = p.epoch; s.luminance_mean = p.luma_mean; s.luminance_peak = p.luma_peak; s.tile_motion = p.motion;
    // The worker's last analysis (-1 and no band until it has heard a hop: no audio, or not yet).
    const float speech = self.speech_now_.load(std::memory_order_relaxed);
    const unsigned band = speech >= 0 ? self.band_now_.load(std::memory_order_relaxed) : audio::SpectralTracker::kBands;
    s.speech_probability = speech;
    s.dominant_band = band;
    s.dominant_band_hz = band < audio::SpectralTracker::kBands ? audio::SpectralTracker::kBandCentres[band] : -1.0f;
    s.dominant_band_db = band < audio::SpectralTracker::kBands ? self.band_db_now_.load(std::memory_order_relaxed) : -144.0f;
    s.flags = (p.cut ? kFrameSceneCut : 0u) | (p.measured ? kFrameMeasured : 0u);
    s.scene_cut_pts_100ns = p.cut ? p.cut_pts : -1;
    self.signature_count_.store(s.sequence + 1, std::memory_order_relaxed);
    if (self.signatures_) {
        if (auto* slot = self.signatures_->AcquireWriteSlot()) { *slot = s; (void)self.signatures_->CommitWriteSlot(); }
        else self.signatures_lost_.fetch_add(1, std::memory_order_relaxed); // Nobody polled: the newest is dropped.
    }
    if (self.options_.frame_callback) {
        if (self.pending_count_ == kPendingSignatures) self.signatures_lost_.fetch_add(1, std::memory_order_relaxed);
        else self.pending_signatures_[self.pending_count_++] = s;
    }
}
void PlaybackCoordinator::FlushSignatures() noexcept {
    // After the presentation pass: the host's callback may give commands (the renderer is between passes).
    for (unsigned i = 0; i < pending_count_; ++i) options_.frame_callback(options_.frame_context, pending_signatures_[i]);
    pending_count_ = 0;
}
ai::IntentStatus PlaybackCoordinator::Track(bool on) noexcept {
    if (on && (music_ || !video_.id)) { Status("Tracking · no picture to follow", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f); return ai::IntentStatus::Unavailable; }
    if (!on && !renderer_.Tracking()) return ai::IntentStatus::NoContext;
    renderer_.SetTracking(on);
    track_events_seen_ = renderer_.TrackEvents(); tracking_shown_ = on;
    if (!on) Status("Tracking off", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
    return ai::IntentStatus::Ok;
}
ai::IntentStatus PlaybackCoordinator::NextTarget() noexcept {
    if (music_ || !video_.id) { Status("Next target \xc2\xb7 no picture to follow", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f); return ai::IntentStatus::Unavailable; }
    if (!renderer_.TargetsReady()) { Status("Next target \xc2\xb7 no object perception here", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f); return ai::IntentStatus::Unavailable; }
    if (!renderer_.NextTarget()) {
        // Nothing perceived yet (perception runs on a grid every 100 ms, and every grid while tracking), or the target is the only object.
        Status(renderer_.Tracking() && renderer_.Objects().object_count ? "Next target \xc2\xb7 the only one in view" : "Next target \xc2\xb7 no object in view",
               ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f);
        return renderer_.Objects().object_count ? ai::IntentStatus::NoContext : ai::IntentStatus::NotFound;
    }
    track_events_seen_ = renderer_.TrackEvents(); tracking_shown_ = true; next_target_cue_ = true;
    SyncAudioFocus(); // The beam turns with the view at once.
    Status("Next target", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
    return ai::IntentStatus::Ok;
}
ai::IntentStatus PlaybackCoordinator::Saccade(bool on) noexcept {
    if (!on) {
        if (!saccade_) return ai::IntentStatus::NoContext;
        saccade_ = false; Status("Audio saccade off", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
        return ai::IntentStatus::Ok;
    }
    if (music_ || !video_.id || !locator_.Configured()) {
        Status("Audio saccade \xc2\xb7 needs stereo sound and a picture", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f);
        return ai::IntentStatus::Unavailable;
    }
    audio::SoundEvent e; saccade_seen_ = locator_.Latest(e) ? e.sequence : 0; // Only sounds from now on.
    saccade_ = true; Status("Audio saccade \xc2\xb7 looking at sounds", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
    return ai::IntentStatus::Ok;
}
void PlaybackCoordinator::PumpSaccade() noexcept {
    if (options_.test_saccade && clock_started_.load(std::memory_order_acquire)) {
        // "look at sound" at 1 s; the view read 0.5 s after each burst (2 s right, 4 s left).
        const auto at = PrecisePosition();
        if (saccade_run_.command < 0 && at >= 10000000) saccade_run_.command = static_cast<int>(Command("look at sound").status);
        if (saccade_run_.right_x < 0 && at >= 25000000) saccade_run_.right_x = renderer_.ShownX();
        if (saccade_run_.left_x < 0 && at >= 45000000) { saccade_run_.left_x = renderer_.ShownX(); saccade_run_.zoom = renderer_.ShownZoom(); }
    }
    // A new sound: the view jumps toward it (a saccade), shown at once through the pictures already converted (live zoom).
    if (!saccade_) return;
    audio::SoundEvent e;
    if (!locator_.Latest(e) || e.sequence == saccade_seen_) return;
    saccade_seen_ = e.sequence;
    if (renderer_.Tracking()) return; // Tracking keeps the view on its target.
    const float zoom = renderer_.Zoom() <= 1.0f ? 2.0f : renderer_.Zoom(), half = 0.5f / zoom;
    // The direction's sine to the picture through a 90-degree field of view: x = 1/2 + tan(asin a) / 2.
    const double a = (std::clamp)(static_cast<double>(e.azimuth), -0.999, 0.999);
    const float x = (std::clamp)(static_cast<float>(0.5 + 0.5 * std::tan(std::asin(a))), half, 1.0f - half);
    renderer_.SetZoom(zoom, x, renderer_.ZoomY(), false); ++saccades_;
    saccade_run_.saccades = saccades_; (saccades_ == 1 ? saccade_run_.right_azimuth : saccade_run_.left_azimuth) = e.azimuth;
    SyncAudioFocus();
    Status(e.azimuth > 0.15f ? "Looking at the sound \xc2\xb7 right" : e.azimuth < -0.15f ? "Looking at the sound \xc2\xb7 left" : "Looking at the sound \xc2\xb7 ahead",
           ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
}
void PlaybackCoordinator::PumpTracking() noexcept {
    // Each UI pass: the audio focus follows the framing on screen, and the tracker's changes become status cues.
    if (renderer_.Tracking()) {
        dsp_controls_.zoom.store(renderer_.Zoom(), std::memory_order_relaxed);
        dsp_controls_.zoom_x.store(renderer_.ShownX(), std::memory_order_relaxed);
    }
    if (options_.test_tracking) {
        // T at 1.0 s of media, through the window's own key handling.
        if (!track_pressed_ && clock_started_.load(std::memory_order_acquire) && PrecisePosition() >= 10000000) {
            track_pressed_ = true; renderer_.Deliver(WM_KEYDOWN, 'T', 0);
        }
        const auto pts = renderer_.ShownPts();
        if (track_pressed_ && pts != track_last_pts_ && track_run_.count < TrackRun::kMax) {
            track_last_pts_ = pts;
            track_run_.samples[track_run_.count++] = {pts, renderer_.ShownX(), renderer_.ShownY(), renderer_.Zoom(),
                                                      dsp_controls_.zoom_x.load(std::memory_order_relaxed), static_cast<std::uint8_t>(renderer_.TrackState().state),
                                                      renderer_.TrackedPts(), renderer_.TrackState().x, renderer_.ShownZoom()};
        }
        track_run_.events = renderer_.TrackEvents();
    }
    if (options_.test_vibrometry && clock_started_.load(std::memory_order_acquire)) {
        // T at 1.0 s; at 6.0 s the readout, tracking off, micro-motion on; at 11.5 s the readout (the warm grids').
        auto& run = vibro_run_;
        const auto position = PrecisePosition();
        const auto take = [this](VibrometryRun::Reading& r) {
            const auto& v = renderer_.VibrometryNow();
            r.taken = true; r.bpm = v.vital_bpm; r.confidence = v.vital_confidence; r.seconds = v.seconds; r.rate = v.rate; r.readouts = renderer_.VibrometryReadouts();
            r.channel = v.vital_channel;
            std::memcpy(r.text, renderer_.VibrometryReadout(), sizeof(r.text)); r.text[sizeof(r.text) - 1] = 0;
        };
        if (!run.tracked && position >= 10000000) { run.tracked = true; renderer_.Deliver(WM_KEYDOWN, 'T', 0); }
        if (run.tracked && !run.tracking.taken && position >= 60000000) {
            take(run.tracking);
            renderer_.Deliver(WM_KEYDOWN, 'T', 0); // Off.
            run.micro_on = SetMicroMotion(true);
        }
        if (run.tracking.taken && !run.micro.taken && position >= 115000000) take(run.micro);
    }
    if (options_.test_next_target && clock_started_.load(std::memory_order_acquire)) {
        // T at 1.0 s, Tab on the very next picture shown (2026-11-01), Tab at 2.0 s (the window's own key handling), "cible
        // suivante" at 3.5 s; a snapshot 0.6 s of media after each.
        static constexpr std::int64_t kAt[TargetRun::kActions]{10000000, 10000000, 20000000, 35000000};
        auto& run = target_run_;
        const auto position = PrecisePosition();
        const bool next_picture = run.actions != 1 || (run.t_pts != INT64_MIN && renderer_.ShownPts() != run.t_pts);
        if (run.actions < TargetRun::kActions && position >= kAt[run.actions] && next_picture) {
            if (run.actions == 0) { run.warm_grids = renderer_.WarmGrids(); renderer_.Deliver(WM_KEYDOWN, 'T', 0); run.t_pts = renderer_.ShownPts(); }
            else if (run.actions < 3) {
                const auto moves = renderer_.Retargets();
                if (run.actions == 1) { run.tab_delay = renderer_.ShownPts() - run.t_pts; run.tab_objects = renderer_.Objects().object_count; }
                renderer_.Deliver(WM_KEYDOWN, VK_TAB, 0);
                if (run.actions == 1) { run.tab_moved = renderer_.Retargets() == moves + 1; run.tab_x = renderer_.ZoomX(); run.tab_y = renderer_.ZoomY(); }
            }
            else run.command = static_cast<int>(Command("cible suivante").status);
            ++run.actions;
        }
        if (run.snapshots < run.actions && position >= kAt[run.snapshots] + 6000000) {
            auto& s = run.after[run.snapshots++];
            const auto& st = renderer_.TrackState(); const auto& r = renderer_.Objects();
            s.pts = renderer_.ShownPts(); s.shown_x = renderer_.ShownX(); s.shown_y = renderer_.ShownY(); s.zoom = renderer_.Zoom();
            s.audio_x = dsp_controls_.zoom_x.load(std::memory_order_relaxed);
            s.state = static_cast<std::uint8_t>(st.state); s.tracked_x = st.x; s.tracked_y = st.y; s.target_id = renderer_.TargetId();
            s.objects = (std::min)(r.object_count, 4u);
            for (unsigned i = 0; i < s.objects; ++i) { s.ids[i] = r.objects[i].track_id; s.x[i] = r.objects[i].x; s.y[i] = r.objects[i].y; }
        }
        run.retargets = renderer_.Retargets();
        // S (the window's own key handling) while tracking: a snapshot, written by the snapshot thread while the pictures go on.
        if (run.actions == TargetRun::kActions && !run.snapshot_pressed && position >= 42000000) {
            run.readback_before = renderer_.SnapshotReadbackBytes(); renderer_.SnapshotSize(run.window_width, run.window_height);
            run.snapshot_pressed = true; run.snapshot_target = renderer_.TargetId(); renderer_.Deliver(WM_KEYDOWN, 'S', 0);
        }
        SnapshotRecord shot;
        if (run.snapshot_pressed && !run.snapshot_written && LastSnapshot(shot) && shot.written) {
            run.readback_after = renderer_.SnapshotReadbackBytes(); run.readback_peak = renderer_.SnapshotReadbackPeak();
            run.snapshot_written = true; run.snapshot_width = shot.width; run.snapshot_height = shot.height;
            std::memcpy(run.snapshot_path, shot.path, sizeof(run.snapshot_path));
        }
    }
    const auto events = renderer_.TrackEvents();
    if (events == track_events_seen_) return;
    track_events_seen_ = events;
    const auto& st = renderer_.TrackState();
    if (!renderer_.Tracking()) {
        if (tracking_shown_) Status(renderer_.TrackLockFailed() ? "Tracking · nothing to follow in the middle" : "Tracking off · the picture changed",
                                    ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f);
        tracking_shown_ = false; SyncAudioFocus();
    } else if (st.state == video::TrackState::Lost) {
        Status("Tracking lost · Holding view", ui::StatusIcon::Warning, ui::StatusTone::Warning, 3.0f);
        if (options_.test_tracking) { static constexpr char kCue[] = "Tracking lost · Holding view"; std::memcpy(track_run_.last_cue, kCue, sizeof(kCue)); }
    }
    else if (st.reacquired) Status("Tracking · target found again", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
    else if (next_target_cue_) next_target_cue_ = false; // The lock "Next target" asked for: its cue stays.
    else Status("Tracking · following the target", ui::StatusIcon::None, ui::StatusTone::Info, 1.2f);
}
ai::IntentStatus PlaybackCoordinator::Snapshot(const char* utf8_path, const video::SnapshotMemory* memory) noexcept {
    if (music_ || !video_.id || !snapshot_thread_) { Status("Snapshot \xc2\xb7 no picture to save", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f); return ai::IntentStatus::Unavailable; }
    const auto n = utf8_path ? std::strlen(utf8_path) : 0;
    if (n >= sizeof(snapshot_path_)) return ai::IntentStatus::Malformed;
    if (snapshot_phase_.load(std::memory_order_acquire) != 0 || renderer_.SnapshotHeld()) {
        Status("Snapshot \xc2\xb7 still saving the last one", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f);
        return ai::IntentStatus::NoContext;
    }
    if (!renderer_.SnapshotPossible()) { Status("Snapshot \xc2\xb7 no picture yet", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f); return ai::IntentStatus::NotFound; }
    std::memcpy(snapshot_path_, utf8_path ? utf8_path : "", n + 1);
    snapshot_to_memory_ = memory != nullptr; if (memory) snapshot_memory_ = *memory; // Read by the snapshot thread after the phase store.
    // Its readback first (2026-11-02: none is held while playing): made by the snapshot thread, of the window's size; the
    // renderer copies the picture with the composite after that (this thread's).
    renderer_.SnapshotSize(snapshot_want_width_, snapshot_want_height_);
    if (!renderer_.SnapshotFits(snapshot_want_width_, snapshot_want_height_)) { snapshot_phase_.store(4, std::memory_order_release); (void)SetEvent(snapshot_wake_); }
    else if (renderer_.RequestSnapshot()) snapshot_phase_.store(1, std::memory_order_relaxed);
    else { Status("Snapshot \xc2\xb7 no picture yet", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f); return ai::IntentStatus::NotFound; }
    return ai::IntentStatus::Ok;
}
void PlaybackCoordinator::SetSnapshotFolder(const char* folder) noexcept {
    const auto n = folder ? std::strlen(folder) : 0;
    snapshot_folder_[0] = 0;
    if (!n || n + 2 > sizeof(snapshot_folder_)) return;
    std::memcpy(snapshot_folder_, folder, n + 1);
    if (snapshot_folder_[n - 1] != '\\' && snapshot_folder_[n - 1] != '/') { snapshot_folder_[n] = '\\'; snapshot_folder_[n + 1] = 0; }
}
bool PlaybackCoordinator::LastSnapshot(SnapshotRecord& out) const noexcept {
    if (!snapshot_done_ || snapshot_phase_.load(std::memory_order_acquire) != 0) return false;
    out = last_snapshot_;
    return true;
}
PlaybackCoordinator::AttachResult PlaybackCoordinator::AttachSubtitles(const char* utf8_path) noexcept {
    if (!attach_ready_ || attach_broken_ || !utf8_path) return AttachResult::Unsupported;
    if (GetCurrentThreadId() == ui_thread_.load(std::memory_order_acquire)) return AttachResult::Busy; // It would wait for itself.
    bool idle = false;
    if (!attaching_.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) return AttachResult::Busy;
    struct Release { std::atomic<bool>& flag; ~Release() { flag.store(false, std::memory_order_release); } } release{attaching_};
    // The file, read through the OS into the read buffer (no stdio stream, nothing allocated).
    wchar_t wide[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, wide, 1024) <= 0 || !attach_read_.Reset()) return AttachResult::Unreadable;
    auto* text = static_cast<char*>(attach_read_.Allocate(kMaxAttachBytes, 16));
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (!text || file == INVALID_HANDLE_VALUE) { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); return AttachResult::Unreadable; }
    LARGE_INTEGER size{}; DWORD read = 0;
    const bool loaded = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= kMaxAttachBytes &&
                        ReadFile(file, text, static_cast<DWORD>(size.QuadPart), &read, nullptr) && read == size.QuadPart;
    CloseHandle(file);
    if (!loaded) return AttachResult::Unreadable;
    // The state: the open sources, the file in the sidecar's place (or after the file's own tracks), parsed with its search index.
    const unsigned slot = attach_next_;
    std::string_view sources[subtitle::SubtitleTrack::kMaxChannels];
    unsigned count = open_source_count_;
    for (unsigned k = 0; k < count; ++k) sources[k] = open_sources_[k];
    sources[attached_slot_] = {text, read}; count = (std::max)(count, attached_slot_ + 1);
    auto& arena = attach_states_[slot];
    if (!arena.Reset()) return AttachResult::Unreadable;
    std::destroy_at(&attached_[slot]); std::construct_at(&attached_[slot]);
    std::destroy_at(&attached_search_[slot]); std::construct_at(&attached_search_[slot]);
    if (!attached_[slot].Prepare(arena, std::span<const std::string_view>{sources, count}) || !attached_[slot].ChannelCues(attached_slot_))
        return AttachResult::Unreadable; // No cue in the file: what is shown is unchanged.
    (void)attached_search_[slot].Prepare(arena, attached_[slot]);
    attached_[slot].Show(1u << attached_slot_);
    // The window's thread rebinds the pipeline on its next pass; waited for here.
    if (!renderer_.RequestSubtitleRebind(&attached_[slot])) return AttachResult::Busy;
    WakePresenter();
    const auto deadline = GetTickCount64() + 3000;
    while (!renderer_.SubtitleRebound(&attached_[slot]) && GetTickCount64() < deadline) Sleep(1);
    if (!renderer_.SubtitleRebound(&attached_[slot])) { attach_broken_ = true; return AttachResult::Timeout; } // The slot may still be taken.
    live_track_.store(&attached_[slot], std::memory_order_release); live_search_.store(&attached_search_[slot], std::memory_order_release);
    attach_next_ = 1 - slot; subtitle_state_ = render::SubtitleStatus::Ready;
    attach_choice_.store(true, std::memory_order_release);
    return AttachResult::Ok;
}
bool PlaybackCoordinator::RequestMemorySnapshot(const video::SnapshotMemory& memory) noexcept {
    if (GetCurrentThreadId() == ui_thread_.load(std::memory_order_acquire)) return false; // It would wait for itself.
    if (memory_request_.load(std::memory_order_acquire) != 0) return false;
    memory_ask_ = memory;
    int idle = 0;
    return memory_request_.compare_exchange_strong(idle, 1, std::memory_order_acq_rel);
}
void PlaybackCoordinator::PumpSnapshot() noexcept {
    // Each UI pass: a readback made is asked to take the next composite; a copy that has landed goes to the snapshot thread
    // with what it shows; a snapshot written is taken back. API 17: a snapshot into a host's memory asked from another thread.
    ui_thread_.store(GetCurrentThreadId(), std::memory_order_release);
    if (memory_request_.load(std::memory_order_acquire) == 1)
        memory_request_.store(Snapshot(nullptr, &memory_ask_) == ai::IntentStatus::Ok ? 2 : 4, std::memory_order_release);
    const int phase = snapshot_phase_.load(std::memory_order_acquire);
    if (phase == 5) {
        if (snapshot_made_ && renderer_.RequestSnapshot()) { snapshot_phase_.store(1, std::memory_order_relaxed); return; }
        snapshot_phase_.store(0, std::memory_order_release);
        if (snapshot_to_memory_) { snapshot_to_memory_ = false; if (memory_request_.load(std::memory_order_acquire) == 2) memory_request_.store(4, std::memory_order_release); }
        Status("Snapshot \xc2\xb7 could not be made", ui::StatusIcon::Warning, ui::StatusTone::Warning, 2.0f);
        return;
    }
    if (phase == 1) {
        auto& image = snapshot_image_;
        if (!renderer_.SnapshotHeld()) {
            // The window grew past the readback before the composite: a larger one.
            renderer_.SnapshotSize(snapshot_want_width_, snapshot_want_height_);
            snapshot_phase_.store(4, std::memory_order_release); (void)SetEvent(snapshot_wake_);
            return;
        }
        if (!renderer_.SnapshotReady(image)) return;
        auto& i = snapshot_info_; i = {};
        i.source = "window"; i.media = snapshot_media_[0] ? snapshot_media_ : nullptr; i.sequence = snapshot_sequence_ + 1; i.pts_100ns = image.pts_100ns;
        i.view_zoom = image.zoom; i.view_x = image.x; i.view_y = image.y;
        i.region[0] = image.region.left; i.region[1] = image.region.top; i.region[2] = image.region.right; i.region[3] = image.region.bottom;
        i.quarter_turns = image.quarter_turns; i.mirrored = image.mirrored; i.level_degrees = image.level; i.auto_level = image.auto_level;
        const auto& horizon = renderer_.Horizon();
        i.horizon_known = horizon.Locked(); i.horizon_degrees = horizon.Target();
        const auto& c = renderer_.PictureControlsNow();
        i.brightness = c.brightness; i.contrast = c.contrast; i.saturation = c.saturation; i.gamma = c.gamma; i.shadow_lift = c.shadow_lift; i.micro_motion = c.micro_motion;
        const auto tier = renderer_.Tier();
        i.reconstruction = tier == render::QualityTier::Neural ? "neural" : tier == render::QualityTier::EdgeDirected ? "edge" : "bilinear";
        i.comparison = image.compare == 1 ? "original-enhanced" : image.compare == 2 ? "pinned-live" : nullptr;
        if (renderer_.TargetsReady()) {
            const auto& r = renderer_.Objects();
            i.object_count = (std::min)(r.object_count, video::kMaxPerceivedObjects);
            for (unsigned k = 0; k < i.object_count; ++k) i.objects[k] = r.objects[k];
            i.target_id = renderer_.Tracking() ? renderer_.TargetId() : 0u;
        }
        // The folder: the one set (its separator ends it), else the default, found by the thread (it may make the folder).
        snapshot_job_default_ = !snapshot_folder_[0];
        std::memcpy(snapshot_job_folder_, snapshot_folder_, sizeof(snapshot_job_folder_));
        snapshot_phase_.store(2, std::memory_order_release);
        (void)SetEvent(snapshot_wake_);
        return;
    }
    if (phase != 3) return;
    renderer_.ReleaseSnapshot();
    const auto& r = snapshot_result_;
    auto& out = last_snapshot_;
    out = {}; out.written = r.written; out.width = r.width; out.height = r.height; out.pts_100ns = r.pts_100ns; out.hash = r.hash;
    std::memcpy(out.path, r.path, sizeof(out.path)); out.path[sizeof(out.path) - 1] = 0;
    if (r.written) { ++snapshots_written_; ++snapshot_sequence_; }
    out.sequence = snapshot_sequence_; snapshot_done_ = true;
    snapshot_phase_.store(0, std::memory_order_release);
    if (snapshot_to_memory_) { // The host's buffer (API 17): no file to name; the waiting caller is told.
        snapshot_to_memory_ = false;
        if (memory_request_.load(std::memory_order_acquire) == 2) memory_request_.store(r.written ? 3 : 4, std::memory_order_release);
        return;
    }
    if (!r.written) { Status("Snapshot \xc2\xb7 could not be written", ui::StatusIcon::Warning, ui::StatusTone::Warning, 2.0f); return; }
    const char* name = out.path;
    for (const char* c = out.path; *c; ++c) if (*c == '/' || *c == '\\') name = c + 1;
    char text[160]; static constexpr char kSaved[] = "Snapshot saved \xc2\xb7 ";
    const auto length = (std::min)(std::strlen(name), sizeof(text) - sizeof(kSaved));
    std::memcpy(text, kSaved, sizeof(kSaved) - 1); std::memcpy(text + sizeof(kSaved) - 1, name, length); text[sizeof(kSaved) - 1 + length] = 0;
    Status(text, ui::StatusIcon::None, ui::StatusTone::Info, 2.0f);
}
void PlaybackCoordinator::SnapshotThread() noexcept {
    // Waits for a job: a readback to make (phase 4, then 5), or a copy to write (phase 2, then 3: the readback released). The
    // UI thread never waits on it.
    for (;;) {
        (void)WaitForSingleObject(snapshot_wake_, INFINITE);
        if (snapshot_stop_.load(std::memory_order_acquire)) return;
        const int phase = snapshot_phase_.load(std::memory_order_acquire);
        if (phase == 4) {
            snapshot_made_ = renderer_.MakeSnapshotReadback(snapshot_want_width_, snapshot_want_height_);
            snapshot_phase_.store(5, std::memory_order_release);
            continue;
        }
        if (phase != 2) continue;
        char folder[512];
        if (!snapshot_job_default_) std::memcpy(folder, snapshot_job_folder_, sizeof(folder));
        else if (!platform::DefaultSnapshotFolder(folder, sizeof(folder))) folder[0] = 0;
        const video::RgbaImage rgba{snapshot_image_.rgba, snapshot_image_.stride};
        (void)video::SaveSnapshotTo(snapshot_encoder_, snapshot_to_memory_ ? &snapshot_memory_ : nullptr, snapshot_path_[0] ? snapshot_path_ : nullptr, folder,
                                    snapshot_stem_, snapshot_image_.width, snapshot_image_.height, &video::SnapshotEncoder::RgbaRows, &rgba, snapshot_info_,
                                    snapshot_result_);
        renderer_.DropSnapshotReadback(); // Written: playing holds no readback.
        snapshot_phase_.store(3, std::memory_order_release);
    }
}
void PlaybackCoordinator::SyncAudioFocus() noexcept {
    // The picture's zoom and its centre across the picture, read by the worker with the other DSP controls.
    dsp_controls_.zoom.store(renderer_.Zoom(), std::memory_order_relaxed);
    dsp_controls_.zoom_x.store(renderer_.ZoomX(), std::memory_order_relaxed);
}
bool PlaybackCoordinator::ConfigureRecogniser(core::LinearMemoryArena& voice, const PlaybackOptions& options) noexcept {
    wake_templates_ = wake_source_templates_ = 0;
    // A custom name (not the built-in one, in either spelling): templates for it, then the shipped set with them as its name.
    std::string_view word = options.wake_word ? std::string_view{options.wake_word} : std::string_view{};
    std::string_view rest;
    const bool builtin = word.empty() || (ai::AvionWakeGate::StripWake(word, rest) && rest.empty());
    if (options.voice_wake && !builtin && ai::CustomWakeWord::Readable(word)) {
        auto* custom = voice.Create<ai::CustomWakeWord>();
        platform::windows::SapiSpeech system;
        ai::SpeechSource source{};
        const ai::SpeechSource* from = options.wake_speech;
        if (!from && options.wake_system_voices && system.Open()) { source = system.Source(); from = &source; }
        if (custom && custom->Synthesize(voice, word, ai::voice_templates::kSet.settings.scale, from) &&
            recogniser_.Configure(voice, ai::voice_templates::kSet, custom->Templates())) {
            wake_templates_ = static_cast<unsigned>(custom->Templates().size()); wake_source_templates_ = custom->SourceTemplates();
            return true;
        }
    }
    return recogniser_.Configure(voice, ai::voice_templates::kSet);
}
bool PlaybackCoordinator::Hear(std::string_view utf8) noexcept {
    // A host's recognised utterance: its time is the run's (16 kHz frames), for the listening window.
    audio::Utterance utterance{};
    utterance.onset_frame = utterance.end_frame = static_cast<std::uint64_t>((std::max)(Now() - run_began_, std::int64_t{0}) / 625);
    std::string_view command;
    const auto decision = text_gate_.Decide(utf8, utterance, command);
    if (decision == ai::WakeDecision::Ignored) return false;
    if (decision == ai::WakeDecision::Listening) {
        char cue[ui::StatusCue::kMaxBytes]; const auto word = text_gate_.Word();
        std::snprintf(cue, sizeof(cue), "%.*s · Listening…", static_cast<int>(word.empty() ? 5 : word.size()), word.empty() ? "Avion" : word.data());
        Status(cue, ui::StatusIcon::Voice, ui::StatusTone::Listening, static_cast<float>(options_.voice_wake_window_ms) / 1000.0f);
        return true;
    }
    (void)Command(command);
    return true;
}
bool PlaybackCoordinator::SetWake(bool required, bool push_to_talk, const char* word) noexcept {
    // The text gate at once; the voice's gate (its own thread) takes it at the next Prepare.
    if (!text_gate_.SetWord(word ? word : "")) return false;
    text_gate_.SetPolicy(required, push_to_talk);
    options_.voice_wake = required; options_.push_to_talk = push_to_talk; options_.wake_word = word;
    return true;
}
bool PlaybackCoordinator::SetAudioZoom(bool on, bool cue) noexcept {
    if (on && !audio_.id) { if (cue) Status("Audio zoom · no sound to steer", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f); return false; }
    SyncAudioFocus();
    dsp_controls_.audio_zoom.store(on, std::memory_order_relaxed);
    if (cue) Status(!on ? "Audio zoom off" : renderer_.Zoom() > audio::AudioFocus::kStart ? "Audio zoom · the sound follows the picture's zoom"
                                                                                          : "Audio zoom · zoom in to steer the sound",
                    ui::StatusIcon::Audio, ui::StatusTone::Info, 2.0f);
    return true;
}
bool PlaybackCoordinator::SetMicroMotion(bool on, bool cue) noexcept {
    if (on && music_) { if (cue) Status("Micro-motion · no picture", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f); return false; }
    auto p = renderer_.PictureControlsNow(); p.micro_motion = on ? kMicroGain : 1.0f;
    renderer_.SetPictureControls(p);
    if (cue) Status(on ? "Micro-motion · small changes magnified 6×" : "Micro-motion off", ui::StatusIcon::None, ui::StatusTone::Info, 2.0f);
    return true;
}
ui::PictureAudioCures::Settings PlaybackCoordinator::CurrentSettings() const noexcept {
    ui::PictureAudioCures::Settings s{};
    const auto& p = renderer_.PictureControlsNow();
    s.picture = {p.brightness, p.contrast, p.saturation, p.gamma, p.shadow_lift, p.micro_motion};
    for (unsigned b = 0; b < audio::MasteringEq::kBands; ++b) s.audio.eq_db[b] = dsp_controls_.eq_db[b].load(std::memory_order_relaxed);
    s.audio.clarity = dsp_controls_.clarity.load(std::memory_order_relaxed);
    s.audio.notch_killer = dsp_controls_.notch_killer.load(std::memory_order_relaxed) ? 1u : 0u;
    s.audio.speech_ducking = dsp_controls_.speech_ducking.load(std::memory_order_relaxed) ? 1u : 0u;
    s.audio.iso_loudness = dsp_controls_.iso_loudness.load(std::memory_order_relaxed) ? 1u : 0u;
    s.audio.audio_zoom = dsp_controls_.audio_zoom.load(std::memory_order_relaxed) ? 1u : 0u;
    return s;
}
void PlaybackCoordinator::ApplySettings(const ui::PictureAudioCures::Settings& s) noexcept {
    renderer_.SetPictureControls({s.picture.brightness, s.picture.contrast, s.picture.saturation, s.picture.gamma, s.picture.shadow_lift,
                                  s.picture.micro_motion});
    for (unsigned b = 0; b < audio::MasteringEq::kBands; ++b) dsp_controls_.eq_db[b].store(s.audio.eq_db[b], std::memory_order_relaxed);
    dsp_controls_.clarity.store(s.audio.clarity, std::memory_order_relaxed);
    dsp_controls_.notch_killer.store(s.audio.notch_killer != 0, std::memory_order_relaxed);
    dsp_controls_.speech_ducking.store(s.audio.speech_ducking != 0, std::memory_order_relaxed);
    dsp_controls_.iso_loudness.store(s.audio.iso_loudness != 0, std::memory_order_relaxed);
    dsp_controls_.audio_zoom.store(s.audio.audio_zoom != 0, std::memory_order_relaxed);
}
ai::IntentStatus PlaybackCoordinator::Cure(ai::IntentKind kind) noexcept {
    if (kind == ai::IntentKind::CureDark ? music_ : !audio_.id) {
        Status(kind == ai::IntentKind::CureDark ? "No picture to brighten" : "No sound to clean", ui::StatusIcon::Warning, ui::StatusTone::Warning, 1.5f);
        return ai::IntentStatus::Rejected;
    }
    auto s = CurrentSettings();
    if (!cures_.Apply(kind, s)) return ai::IntentStatus::Rejected;
    ApplySettings(s);
    Status(ui::PictureAudioCures::Describe(kind), kind == ai::IntentKind::CureDark ? ui::StatusIcon::None : ui::StatusIcon::Audio, ui::StatusTone::Info, 3.0f);
    return ai::IntentStatus::Ok;
}
ai::IntentStatus PlaybackCoordinator::UndoCure() noexcept {
    auto s = CurrentSettings();
    if (!cures_.Undo(s)) return ai::IntentStatus::NoContext;
    ApplySettings(s);
    Status("Undone · back as it was", ui::StatusIcon::None, ui::StatusTone::Info, 2.0f);
    return ai::IntentStatus::Ok;
}
bool PlaybackCoordinator::SetNight(bool on, bool cue) noexcept {
    dsp_controls_.night.store(on, std::memory_order_relaxed);
    ui_events_.Record(Now(), core::FlightEvent::NightMode, on ? 1 : 0);
    if (cue) Status(on ? "Night mode \xc2\xb7 quiet speech lifted, loud bursts tamed" : "Night mode off", ui::StatusIcon::Audio, ui::StatusTone::Info, 2.0f);
    return true;
}
PlaybackCoordinator::LoudnessReading PlaybackCoordinator::LoudnessNow() const noexcept {
    return {lufs_momentary_.load(std::memory_order_relaxed), lufs_short_.load(std::memory_order_relaxed), true_peak_.load(std::memory_order_relaxed),
            night_gain_.load(std::memory_order_relaxed), limiter_db_.load(std::memory_order_relaxed)};
}
void PlaybackCoordinator::BookmarksChanged() noexcept {
    ui::BookmarkMarks marks;
    const auto duration = Bounds().duration_100ns;
    for (unsigned i = 0; i < bookmarks_.Count(); ++i) marks.Set(i, timeline::LoopRegion::Fraction(bookmarks_.At(i), duration));
    renderer_.SetBookmarkMarks(marks);
}
void PlaybackCoordinator::BookmarkCue(const char* what, unsigned number, std::int64_t at) noexcept {
    // "Bookmark 3 · 0:12.4", "Bookmark 2 of 5 · 0:12.4".
    char text[64]; unsigned n = 0;
    const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; text[n] = 0; };
    const auto whole = [&](unsigned v) { char d[12]; unsigned k = 0; do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && k < 11); while (k && n + 1 < sizeof(text)) text[n++] = d[--k]; text[n] = 0; };
    put(what); whole(number);
    if (std::strcmp(what, "Bookmark ") == 0 && bookmarks_.Count() > 1) { put(" of "); whole(bookmarks_.Count()); }
    put(" \xc2\xb7 "); PutClock(text, n, sizeof(text), at);
    Status(text, ui::StatusIcon::None, ui::StatusTone::Info, 1.5f);
}
ai::IntentStatus PlaybackCoordinator::AddBookmark() noexcept {
    if (!seekable_) { Status("Bookmarks \xc2\xb7 this media cannot seek", ui::StatusIcon::Warning, ui::StatusTone::Warning); return ai::IntentStatus::Unavailable; }
    const auto at = PrecisePosition();
    const auto before = bookmarks_.Count();
    const int index = bookmarks_.Add(at);
    if (index < 0) { Status("Bookmarks \xc2\xb7 32 is the most", ui::StatusIcon::Warning, ui::StatusTone::Warning); return ai::IntentStatus::OutOfRange; }
    (void)renderer_.PinReference(); // The frame on screen, for Pinned | Live.
    BookmarksChanged();
    const auto number = static_cast<unsigned>(index) + 1;
    BookmarkCue(bookmarks_.Count() > before ? "Bookmarked " : "Already bookmark ", number, bookmarks_.At(static_cast<unsigned>(index)));
    ui_events_.Record(Now(), core::FlightEvent::Bookmark, 1, number, bookmarks_.At(static_cast<unsigned>(index)));
    return ai::IntentStatus::Ok;
}
ai::IntentStatus PlaybackCoordinator::GoToBookmark(unsigned number) noexcept {
    if (!number) return NextBookmark();
    if (!seekable_ || !bookmarks_.Count()) { Status("No bookmarks yet \xc2\xb7 B adds one", ui::StatusIcon::Warning, ui::StatusTone::Warning); return ai::IntentStatus::NoContext; }
    if (number > bookmarks_.Count()) { Status("No such bookmark", ui::StatusIcon::Warning, ui::StatusTone::Warning); return ai::IntentStatus::OutOfRange; }
    const auto at = bookmarks_.At(number - 1);
    if (!RequestSeek(at)) return ai::IntentStatus::Rejected;
    BookmarkCue("Bookmark ", number, at);
    ui_events_.Record(Now(), core::FlightEvent::Bookmark, 2, number, at);
    return ai::IntentStatus::Ok;
}
ai::IntentStatus PlaybackCoordinator::NextBookmark() noexcept {
    const int next = bookmarks_.Next(Position());
    if (next < 0) { Status(bookmarks_.Count() ? "No bookmark after this" : "No bookmarks yet \xc2\xb7 B adds one", ui::StatusIcon::Warning, ui::StatusTone::Warning); return bookmarks_.Count() ? ai::IntentStatus::NotFound : ai::IntentStatus::NoContext; }
    return GoToBookmark(static_cast<unsigned>(next) + 1);
}
ai::IntentStatus PlaybackCoordinator::PreviousBookmark() noexcept {
    const int previous = bookmarks_.Previous(Position());
    if (previous < 0) { Status(bookmarks_.Count() ? "No bookmark before this" : "No bookmarks yet \xc2\xb7 B adds one", ui::StatusIcon::Warning, ui::StatusTone::Warning); return bookmarks_.Count() ? ai::IntentStatus::NotFound : ai::IntentStatus::NoContext; }
    return GoToBookmark(static_cast<unsigned>(previous) + 1);
}
ai::IntentStatus PlaybackCoordinator::PinFrame() noexcept {
    if (music_ || !renderer_.PinReference()) { Status("Pin \xc2\xb7 no picture to pin", ui::StatusIcon::Warning, ui::StatusTone::Warning); return ai::IntentStatus::Unavailable; }
    char text[64]; unsigned n = 0;
    const auto put = [&](const char* part) { while (*part && n + 1 < sizeof(text)) text[n++] = *part++; text[n] = 0; };
    put("Frame pinned \xc2\xb7 "); PutClock(text, n, sizeof(text), renderer_.ReferencePts()); put(" \xc2\xb7 V compares");
    Status(text, ui::StatusIcon::Device, ui::StatusTone::Info, 2.0f);
    ui_events_.Record(Now(), core::FlightEvent::Bookmark, 3, 0, renderer_.ReferencePts());
    return ai::IntentStatus::Ok;
}
ai::IntentStatus PlaybackCoordinator::ComparePinned() noexcept {
    if (!renderer_.ReferencePinned()) { Status("Comparison \xc2\xb7 pin a frame first (Ctrl+B)", ui::StatusIcon::Warning, ui::StatusTone::Warning); return ai::IntentStatus::NoContext; }
    renderer_.SetCompare(render::D3D12Renderer::Compare::Pinned);
    return ai::IntentStatus::Ok;
}
void PlaybackCoordinator::Close() noexcept {
    UiPhase(Phase::Closing);
    stop_.store(true); teardown_.store(true); voice_stop_.store(true);
    if (voice_thread_) { (void)WaitForSingleObject(voice_thread_, INFINITE); CloseHandle(voice_thread_); voice_thread_ = nullptr; }
    if (worker_) { (void)WaitForSingleObject(worker_, INFINITE); CloseHandle(worker_); worker_ = nullptr; }
    if (snapshot_thread_) { // A snapshot being written is finished first (it reads the renderer's readback).
        snapshot_stop_.store(true, std::memory_order_release); (void)SetEvent(snapshot_wake_);
        (void)WaitForSingleObject(snapshot_thread_, INFINITE); CloseHandle(snapshot_thread_); snapshot_thread_ = nullptr;
    }
    if (snapshot_wake_) { CloseHandle(snapshot_wake_); snapshot_wake_ = nullptr; }
    // Worker is joined: ownership of the queues/ledger is now handed to teardown.
    if (surface_returns_) { unsigned count = 0; (void)decoder_.PollReturns(*surface_returns_, 64, count); }
    if (frames_) while (const auto* f = frames_->PeekReadSlot()) {
        (void)decoder_.ReturnSurface({f->surface_id, f->surface_generation, f->playback_epoch, 2, 0}); (void)frames_->ReleaseReadSlot();
    }
    if (packet_returns_) while (const auto* p = packet_returns_->PeekReadSlot()) { (void)parser_.ReturnPacket(*p); (void)packet_returns_->ReleaseReadSlot(); }
    if (packets_) while (const auto* p = packets_->PeekReadSlot()) { (void)parser_.ReturnPacket(*p); (void)packets_->ReleaseReadSlot(); }
    if (!renderer_.Close()) { decoder_.QuarantineGpuResources(); Fail(30, renderer_.LastError()); }
    // A warm session's decoder waits for the next file (which restarts it, or closes it if it does not fit).
    if (!warm_) (void)decoder_.DrainAndClose();
    for (HANDLE* h : {&pace_timer_, &pace_picture_, &pace_fence_, &worker_timer_, &worker_wake_}) if (*h) { CloseHandle(*h); *h = nullptr; }
    UiPhase(Phase::Joined);
}
}
