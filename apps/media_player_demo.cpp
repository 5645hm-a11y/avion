#include "media/engine/PlaybackCoordinator.hpp"
#include "media/platform/windows/SessionFiles.hpp"
#include "allocation_probe.hpp"
#include "horizon_scene.hpp"
#include "loopback_http_server.hpp"
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <cstdio>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <cwchar>
#include <cstdlib>
#include <cmath>
namespace {
enum class Mode { Play, Test, Trim, Underrun, Hiccup, Pause, Hdr, Hud, Seek, SeekForward, SeekMicro, Scrub, Dsp, Subtitles, SubtitleScrub, SubtitleOverflow, SubtitleMissing, Resize, Surround, Surround71, Neural, Quality, LongForm, TracksSurround,
                  DeviceLoss, DeviceLossPaused, AudioLoss, AudioSteal, Volume, Intents, Palette, Fixture, FixtureSearch, FixtureHdr, Anamorphic, Voice, Tracks,
                  Isolation, Chapters, FollowUps, NeuralReuse, PausedQuality, Resume, Speed, Zoom, Music, MusicSession, ZoomLive, AbLoop, Telemetry, Bookmarks, Tracking, Saccade, Pacing,
                  Rotation, WindowStall, AutoHorizon, CropBars, NextTarget, Vibrometry, VibrometryChroma, Stabilize, Attach, RemoteSeek };
// Hardware faults for the recovery tests, raised from a thread of their own at a fixed time after Run starts: real
// events, as another process or the driver would cause them, never a simulated error code.
// - Gpu: ID3D12Device5::RemoveDevice on the renderer's device (what a TDR or driver restart does to the process).
// - AudioInjected (--test-audio-loss, the CTest default): the sink's stream is invalidated in-process
//   (PlaybackCoordinator::InjectAudioFault: AUDCLNT_E_DEVICE_INVALIDATED where GetCurrentPadding reports it, then every
//   open refused with AUDCLNT_E_DEVICE_IN_USE for the hold). The machine's audio device is not touched: other
//   applications keep playing.
// - Audio (--test-exclusive-steal, manual only): another stream takes the default render endpoint in exclusive mode and
//   holds it. Windows invalidates the player's shared stream (AUDCLNT_E_DEVICE_INVALIDATED) and refuses new shared
//   streams (AUDCLNT_E_DEVICE_IN_USE) until it is released. It takes the device from every application on the machine
//   (a media player then fails with 0xC00D4E85, "the audio device is in use"), so CTest never runs it unless the build
//   asks for it (MEDIA_TEST_EXCLUSIVE_STEAL).
// - Desktop (--test-isolation): stray desktop input on the test window, from another thread, through the window's queue
//   as the desktop's input arrives (not tagged by the harness): a drag and clicks on the timeline and the transport
//   button, keys (Space, arrows, End, N, F, F11, Up, '/'), the wheel, and the WM_MOUSEACTIVATE a real click sends.
struct Fault {
    enum class Kind { None, Gpu, Audio, AudioInjected, Desktop } kind = Kind::None;
    unsigned at_ms = 0, hold_ms = 0;
    HWND window = nullptr;
    LRESULT activation = 0; // Desktop: what the window answered to WM_MOUSEACTIVATE.
    ID3D12Device* device = nullptr; // Borrowed, no reference: a held reference would itself be a leak.
    media::engine::PlaybackCoordinator* player = nullptr;
    std::atomic<bool> stop{false}, running{false};
    HRESULT result = E_PENDING;
    bool held = false; unsigned attempts = 0;
};
bool Wait(const Fault& fault, ULONGLONG until) {
    while (GetTickCount64() < until) { if (fault.stop.load()) return false; Sleep(2); }
    return true;
}
void Inject(Fault& fault) {
    fault.running.store(true); // The CRT's per-thread setup is done: measurement may start.
    const auto begin = GetTickCount64();
    if (!Wait(fault, begin + fault.at_ms)) return;
    if (fault.kind == Fault::Kind::Gpu) {
        ID3D12Device5* device = nullptr;
        fault.result = fault.device->QueryInterface(IID_PPV_ARGS(&device));
        if (SUCCEEDED(fault.result)) { device->RemoveDevice(); device->Release(); }
        return;
    }
    if (fault.kind == Fault::Kind::AudioInjected) {
        fault.player->InjectAudioFault(AUDCLNT_E_DEVICE_INVALIDATED, fault.hold_ms);
        fault.result = S_OK; fault.held = true; fault.attempts = 1;
        return;
    }
    if (fault.kind == Fault::Kind::Desktop) {
        RECT client{}; (void)GetClientRect(fault.window, &client);
        const int width = client.right, height = client.bottom, timeline = height - 50, transport = height - 90;
        const auto post = [&](UINT message, WPARAM w, LPARAM l) { if (PostMessageW(fault.window, message, w, l)) ++fault.attempts; };
        const auto at = [](int x, int y) { return MAKELPARAM(x, y); };
        for (unsigned round = 0; round < 2 && !fault.stop.load(); ++round) {
            // What a real click on the timeline does first: the system asks the window whether to activate it.
            fault.activation = SendMessageW(fault.window, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(fault.window), MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
            post(WM_MOUSEMOVE, 0, at(width * 3 / 4, timeline)); post(WM_LBUTTONDOWN, MK_LBUTTON, at(width * 3 / 4, timeline));
            post(WM_MOUSEMOVE, MK_LBUTTON, at(width / 4, timeline)); post(WM_LBUTTONUP, 0, at(width / 4, timeline));
            post(WM_LBUTTONDOWN, MK_LBUTTON, at(width / 2, transport)); post(WM_LBUTTONUP, 0, at(width / 2, transport));
            for (const WPARAM key : {WPARAM{VK_SPACE}, WPARAM{VK_RIGHT}, WPARAM{VK_END}, WPARAM{'N'}, WPARAM{'F'}, WPARAM{VK_F11}, WPARAM{VK_UP}})
                post(WM_KEYDOWN, key, 1);
            post(WM_CHAR, L'/', 1);
            post(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), at(width / 2, height / 2));
            fault.result = S_OK;
            if (!Wait(fault, GetTickCount64() + 700)) return;
        }
        return;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr; IMMDevice* endpoint = nullptr; IAudioClient* client = nullptr; WAVEFORMATEX* mix = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint);
    if (SUCCEEDED(hr)) hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
    WAVEFORMATEXTENSIBLE format{};
    if (SUCCEEDED(hr)) {
        // An exclusive-mode PCM format the endpoint takes, at its mix rate and channel count.
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; format.Format.nChannels = mix->nChannels; format.Format.nSamplesPerSec = mix->nSamplesPerSec;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX); format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        format.dwChannelMask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        for (const WORD bits : {WORD{16}, WORD{24}, WORD{32}}) {
            format.Format.wBitsPerSample = bits == 24 ? 32 : bits; format.Samples.wValidBitsPerSample = bits;
            format.Format.nBlockAlign = static_cast<WORD>(format.Format.nChannels * format.Format.wBitsPerSample / 8);
            format.Format.nAvgBytesPerSec = format.Format.nBlockAlign * format.Format.nSamplesPerSec;
            if ((hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr)) == S_OK) break;
        }
    }
    REFERENCE_TIME period = 0, minimum = 0;
    if (SUCCEEDED(hr)) hr = client->GetDevicePeriod(&period, &minimum);
    // The first request may itself be refused while Windows disconnects the shared streams: retry for up to 2 s.
    const auto deadline = GetTickCount64() + 2000;
    while (SUCCEEDED(hr) && !fault.held && GetTickCount64() < deadline && !fault.stop.load()) {
        ++fault.attempts;
        const HRESULT exclusive = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, period, period, &format.Format, nullptr);
        fault.result = exclusive;
        if (SUCCEEDED(exclusive)) { fault.held = true; break; }
        client->Release(); client = nullptr; Sleep(20);
        hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    }
    if (FAILED(hr)) fault.result = hr;
    if (fault.held) (void)Wait(fault, GetTickCount64() + fault.hold_ms);
    if (mix) CoTaskMemFree(mix);
    if (client) client->Release(); // Releases the exclusive stream: shared streams may open again.
    if (endpoint) endpoint->Release();
    if (enumerator) enumerator->Release();
    if (SUCCEEDED(com)) CoUninitialize();
}
void AudioKey(void* context, unsigned key) noexcept {
    auto& player = *static_cast<media::engine::PlaybackCoordinator*>(context);
    if (key == 'D') player.SetClarityLevel((player.ClarityLevel() + 1) % 4);
    if (key == 'S' && player.Renderer().ModifierDown(VK_SHIFT)) player.SetSpatialAudio(!player.SpatialAudio()); // Shift+S (S: a snapshot).
    // Volume in 5 % steps (3 dB each on the engine's taper), kept on the 5 % grid so repeated presses never drift.
    if (key == VK_UP || key == VK_DOWN) player.SetVolume(static_cast<float>(std::lround(player.Volume() * 20) + (key == VK_UP ? 1 : -1)) / 20);
    if (key == 'M') player.SetMuted(!player.Muted());
}
Mode Parse(const wchar_t* flag) {
    if (std::wcscmp(flag, L"--test") == 0) return Mode::Test;
    if (std::wcscmp(flag, L"--test-trim") == 0) return Mode::Trim;
    if (std::wcscmp(flag, L"--test-underrun") == 0) return Mode::Underrun;
    if (std::wcscmp(flag, L"--test-hiccup") == 0) return Mode::Hiccup;
    if (std::wcscmp(flag, L"--test-pause") == 0) return Mode::Pause;
    if (std::wcscmp(flag, L"--test-hdr") == 0) return Mode::Hdr;
    if (std::wcscmp(flag, L"--test-anamorphic") == 0) return Mode::Anamorphic;
    if (std::wcscmp(flag, L"--test-hud") == 0) return Mode::Hud;
    if (std::wcscmp(flag, L"--test-seek") == 0) return Mode::Seek;
    if (std::wcscmp(flag, L"--test-seek-forward") == 0) return Mode::SeekForward;
    if (std::wcscmp(flag, L"--test-seek-micro") == 0) return Mode::SeekMicro;
    if (std::wcscmp(flag, L"--test-scrub") == 0) return Mode::Scrub;
    if (std::wcscmp(flag, L"--test-dsp") == 0) return Mode::Dsp;
    if (std::wcscmp(flag, L"--test-subtitles") == 0) return Mode::Subtitles;
    if (std::wcscmp(flag, L"--test-subtitle-scrub") == 0) return Mode::SubtitleScrub;
    if (std::wcscmp(flag, L"--test-subtitle-overflow") == 0) return Mode::SubtitleOverflow;
    if (std::wcscmp(flag, L"--test-subtitle-missing") == 0) return Mode::SubtitleMissing;
    if (std::wcscmp(flag, L"--test-resize") == 0) return Mode::Resize;
    if (std::wcscmp(flag, L"--test-surround") == 0) return Mode::Surround;
    if (std::wcscmp(flag, L"--test-surround71") == 0) return Mode::Surround71;
    if (std::wcscmp(flag, L"--test-neural") == 0) return Mode::Neural;
    if (std::wcscmp(flag, L"--test-quality") == 0) return Mode::Quality;
    if (std::wcscmp(flag, L"--test-longform") == 0) return Mode::LongForm;
    if (std::wcscmp(flag, L"--test-remote-seek") == 0) return Mode::RemoteSeek;
    if (std::wcscmp(flag, L"--test-device-loss") == 0) return Mode::DeviceLoss;
    if (std::wcscmp(flag, L"--test-device-loss-paused") == 0) return Mode::DeviceLossPaused;
    if (std::wcscmp(flag, L"--test-audio-loss") == 0) return Mode::AudioLoss;
    if (std::wcscmp(flag, L"--test-exclusive-steal") == 0) return Mode::AudioSteal;
    if (std::wcscmp(flag, L"--test-volume") == 0) return Mode::Volume;
    if (std::wcscmp(flag, L"--test-isolation") == 0) return Mode::Isolation;
    if (std::wcscmp(flag, L"--test-chapters") == 0) return Mode::Chapters;
    if (std::wcscmp(flag, L"--test-intents") == 0) return Mode::Intents;
    if (std::wcscmp(flag, L"--test-followups") == 0) return Mode::FollowUps;
    if (std::wcscmp(flag, L"--test-resume") == 0) return Mode::Resume;
    if (std::wcscmp(flag, L"--test-speed") == 0) return Mode::Speed;
    if (std::wcscmp(flag, L"--test-zoom") == 0) return Mode::Zoom;
    if (std::wcscmp(flag, L"--test-rotation") == 0) return Mode::Rotation;
    if (std::wcscmp(flag, L"--test-window-stall") == 0) return Mode::WindowStall;
    if (std::wcscmp(flag, L"--test-auto-horizon") == 0) return Mode::AutoHorizon;
    if (std::wcscmp(flag, L"--test-crop-bars") == 0) return Mode::CropBars;
    if (std::wcscmp(flag, L"--test-zoom-live") == 0) return Mode::ZoomLive;
    if (std::wcscmp(flag, L"--test-tracking") == 0) return Mode::Tracking;
    if (std::wcscmp(flag, L"--test-next-target") == 0) return Mode::NextTarget;
    if (std::wcscmp(flag, L"--test-vibrometry") == 0) return Mode::Vibrometry;
    if (std::wcscmp(flag, L"--test-vibrometry-chroma") == 0) return Mode::VibrometryChroma;
    if (std::wcscmp(flag, L"--test-stabilize") == 0) return Mode::Stabilize;
    if (std::wcscmp(flag, L"--test-saccade") == 0) return Mode::Saccade;
    if (std::wcscmp(flag, L"--test-pacing") == 0) return Mode::Pacing;
    if (std::wcscmp(flag, L"--test-attach") == 0) return Mode::Attach;
    if (std::wcscmp(flag, L"--test-ab-loop") == 0) return Mode::AbLoop;
    if (std::wcscmp(flag, L"--test-telemetry") == 0) return Mode::Telemetry;
    if (std::wcscmp(flag, L"--test-bookmarks") == 0) return Mode::Bookmarks;
    if (std::wcscmp(flag, L"--test-music") == 0) return Mode::Music;
    if (std::wcscmp(flag, L"--test-music-session") == 0) return Mode::MusicSession;
    if (std::wcscmp(flag, L"--test-neural-reuse") == 0) return Mode::NeuralReuse;
    if (std::wcscmp(flag, L"--test-paused-quality") == 0) return Mode::PausedQuality;
    if (std::wcscmp(flag, L"--test-palette") == 0) return Mode::Palette;
    if (std::wcscmp(flag, L"--test-fixture") == 0) return Mode::Fixture;
    if (std::wcscmp(flag, L"--test-fixture-search") == 0) return Mode::FixtureSearch;
    if (std::wcscmp(flag, L"--test-fixture-hdr") == 0) return Mode::FixtureHdr;
    if (std::wcscmp(flag, L"--test-voice") == 0) return Mode::Voice;
    if (std::wcscmp(flag, L"--test-tracks") == 0) return Mode::Tracks;
    if (std::wcscmp(flag, L"--test-tracks-surround") == 0) return Mode::TracksSurround;
    return Mode::Play;
}
// What a seek cost the decoder: carried on from the decode position or restarted from the sync sample, and the pictures
// before the target it decoded (references) or passed over (neither shown nor referenced).
void PrintWork(const media::engine::PlaybackCoordinator& player, std::uint32_t epoch) {
    const auto* w = player.WorkFor(epoch);
    if (!w) return;
    if (w->adopted) std::printf("  decoder: queue seek, nothing decoded again (landing %s); ", w->kept ? "already converted" : "already decoded");
    else if (w->continued) std::printf("  decoder: continued from the decode position (%u pictures from the sync sample not read again); ", w->resumed);
    else std::printf("  decoder: restarted from the sync sample; ");
    std::printf("%u reference pictures decoded before the target, %u skipped\n", w->references, w->skipped);
}
// The media time a seek's landing image stands for: the target inside the landing frame's interval, else its PTS.
std::int64_t Landing(std::int64_t target, std::int64_t pts, std::int64_t duration) {
    return target > pts && target - pts < duration ? target : pts;
}
// The cues composited into a seek's landing image must be exactly those the track selects at the landing time.
// The scripted targets all fall inside a cue, so an empty selection is a failure too.
bool CaptionsMatch(const media::engine::PlaybackCoordinator& player, const media::render::D3D12Renderer::SubtitleFrame& shown, std::int64_t pts) {
    unsigned expected[media::subtitle::SubtitleTrack::kMaxActive]{};
    const unsigned count = player.Captions().Select(pts, expected);
    bool same = shown.time == pts && shown.count == count && count > 0;
    for (unsigned i = 0; same && i < count; ++i) same = shown.cues[i] == expected[i];
    std::printf("Captions at %.4fs: cue %d of %u shown, %s\n", pts / 1e7, shown.count ? static_cast<int>(shown.cues[0]) : -1,
                shown.count, same ? "match" : "WRONG");
    return same;
}
// --test-intents: commands as a person would say them, in Hebrew and English, run through the engine's dispatcher
// during playback of the 3 s fixture with its three-cue track (cue 1 at 1.4 s holds "עברית", cue 2 at 2.3 s "日本語").
using media::ai::IntentKind; using media::ai::IntentStatus;
constexpr media::engine::ScriptedCommand kIntentScript[]{
    {300, "תנמיך"}, {450, "volume 40"}, {600, "השתק"}, {800, "בטל השתקה"}, {950, "הגבר"},
    {1000, "forward 1 hour"}, {1050, "make coffee"}, {1100, "מצא עברית"}, {1600, "עצור"}, {1800, "חזור שנייה"},
    {2200, "find 日本語"}, {2600, "נגן"}};
struct ExpectedIntent { IntentKind kind; IntentStatus status; float volume; bool muted, paused; int cue; };
constexpr ExpectedIntent kIntentExpected[]{
    {IntentKind::VolumeDown, IntentStatus::Ok, 0.95f, false, false, -1}, {IntentKind::VolumeSet, IntentStatus::Ok, 0.40f, false, false, -1},
    {IntentKind::Mute, IntentStatus::Ok, 0.40f, true, false, -1}, {IntentKind::Unmute, IntentStatus::Ok, 0.40f, false, false, -1},
    {IntentKind::VolumeUp, IntentStatus::Ok, 0.45f, false, false, -1}, {IntentKind::SeekRelative, IntentStatus::OutOfRange, 0.45f, false, false, -1},
    {IntentKind::None, IntentStatus::Unknown, 0.45f, false, false, -1}, {IntentKind::FindSubtitle, IntentStatus::Ok, 0.45f, false, false, 1},
    {IntentKind::Pause, IntentStatus::Ok, 0.45f, false, true, -1}, {IntentKind::SeekRelative, IntentStatus::Ok, 0.45f, false, true, -1},
    {IntentKind::FindSubtitle, IntentStatus::Ok, 0.45f, false, true, 2}, {IntentKind::Play, IntentStatus::Ok, 0.45f, false, false, -1}};
// --test-followups (docs/intents.md, "Follow-ups"): a search for "·" (in cues 1 and 2 of the same track), then its matches
// by "next match", "התוצאה הקודמת" and "את השני", "בטל" (back to where playback was before the last jump), a second back
// and "עוד פעם" (the same again, from there), "what did he say" (the line on screen from its start), a refused ordinal,
// and the split comparison on and off by voice-style commands.
constexpr media::engine::ScriptedCommand kFollowUpScript[]{
    {200, "find ·"}, {500, "next match"}, {800, "התוצאה הקודמת"}, {1100, "את השני"}, {1400, "בטל"}, {1700, "חזור שנייה"},
    {2000, "עוד פעם"}, {2300, "what did he say"}, {2450, "the fifth one"}, {2500, "compare quality"}, {2900, "stop comparing"}};
// cue: the cue the landing must show (-1: the seek's own target; -2: the start of whichever cue it landed on).
struct ExpectedFollowUp { IntentKind kind; IntentStatus status; IntentKind from; int cue; };
constexpr ExpectedFollowUp kFollowUpExpected[]{
    {IntentKind::FindSubtitle, IntentStatus::Ok, IntentKind::None, 1}, {IntentKind::FindSubtitle, IntentStatus::Ok, IntentKind::NextMatch, 2},
    {IntentKind::FindSubtitle, IntentStatus::Ok, IntentKind::PreviousMatch, 1}, {IntentKind::FindSubtitle, IntentStatus::Ok, IntentKind::NthMatch, 2},
    {IntentKind::SeekAbsolute, IntentStatus::Ok, IntentKind::Undo, -1}, {IntentKind::SeekRelative, IntentStatus::Ok, IntentKind::None, -1},
    {IntentKind::SeekRelative, IntentStatus::Ok, IntentKind::Repeat, -1}, {IntentKind::ReplayCue, IntentStatus::Ok, IntentKind::None, -2},
    {IntentKind::FindSubtitle, IntentStatus::OutOfRange, IntentKind::NthMatch, -1}, {IntentKind::CompareOn, IntentStatus::Ok, IntentKind::None, -1},
    {IntentKind::CompareOff, IntentStatus::Ok, IntentKind::None, -1}};
// --test-palette: the same kind of commands typed into the window's command bar, as the key and character messages a
// keyboard produces, in seven languages. Each line opens the bar with `/` and types; Enter follows a little later, so
// the bar and its text are on screen in between. One text is no command and is looked for as a quote (עברית, in cue
// 1); one is found nowhere (the bar refuses and stays until Escape, which must not close the window).
constexpr media::engine::ScriptedTyping kPaletteScript[]{
    {200, L"/音量400\b"}, {330, L"\r"}, // A typo, then Backspace: 音量40.
    {450, L"/пауза"}, {570, L"\r"},
    {700, L"/再生"}, {820, L"\r"},
    {950, L"/עברית"}, {1070, L"\r"}, // Not a command: a quote.
    {1400, L"/zurückspulen 1 Sekunde"}, {1520, L"\r"},
    {1750, L"/xyzzy"}, {1870, L"\r"}, {1990, L"\x1b"},
    {2100, L"/توقف"}, {2220, L"\r"},
    {2350, L"/chercher quiet"}, {2470, L"\r"},
    {2700, L"/reproducir"}, {2820, L"\r"}};
constexpr ExpectedIntent kPaletteExpected[]{
    {IntentKind::VolumeSet, IntentStatus::Ok, 0.40f, false, false, -1}, {IntentKind::Pause, IntentStatus::Ok, 0.40f, false, true, -1},
    {IntentKind::Play, IntentStatus::Ok, 0.40f, false, false, -1}, {IntentKind::FindSubtitle, IntentStatus::Ok, 0.40f, false, false, 1},
    {IntentKind::SeekRelative, IntentStatus::Ok, 0.40f, false, false, -1}, {IntentKind::FindSubtitle, IntentStatus::NotFound, 0.40f, false, false, -1},
    {IntentKind::Pause, IntentStatus::Ok, 0.40f, false, true, -1}, {IntentKind::FindSubtitle, IntentStatus::Ok, 0.40f, false, true, 0},
    {IntentKind::Play, IntentStatus::Ok, 0.40f, false, false, -1}};
// --test-tracks (the --multitrack fixture, docs/tracks.md): audio and subtitle tracks changed while playing by
// conversational commands in Hebrew and English, changes to a track of another sample rate (converted 44.1 -> 48 kHz)
// and of another layout (5.1 folded to stereo), refusals for a refused codec and a language the file does not have,
// a change right behind a seek, and a pause and resume said as sentences.
// Track ids: 2 English (440 Hz), 3 Hebrew (660 Hz), 4 French 44.1 kHz (880 Hz), 5 Spanish AC-3, 9 Arabic 5.1 (550 Hz);
// subtitles 6 Hebrew, 7 English.
constexpr std::uint32_t kNone = media::engine::PlaybackCoordinator::kNoTrack;
constexpr media::engine::ScriptedCommand kTrackScript[]{
    {800, "תעביר לאודיו בעברית"}, {1800, "english subtitles please"}, {2800, "switch subtitles to hebrew"},
    {3800, "can you please switch the audio to english"}, {4600, "audio in french"}, {5200, "spanish audio"},
    {5800, "turn off the subtitles"}, {6500, "next audio track"}, {7200, "go back 3 seconds"}, {7230, "english audio"},
    {8500, "כתוביות בעברית"}, {9500, "German subtitles"}, {10200, "אשמח שתעצור עכשיו"}, {10700, "תמשיך בבקשה"}};
struct ExpectedTrack { IntentKind kind; IntentStatus status; std::uint32_t audio, subtitles; };
// --test-tracks-surround: the same fixture started on the Arabic 5.1 track (folded to a stereo endpoint, or played
// on a 5.1 one), then stereo tracks put on its layout: English (stereo -> 5.1 front pair), French (and 44.1 -> 48 kHz),
// Arabic again (no conversion) and Hebrew.
constexpr media::engine::ScriptedCommand kTrackSurroundScript[]{
    {800, "english audio"}, {2200, "audio in french"}, {3600, "arabic audio"}, {5000, "hebrew audio"}};
constexpr ExpectedTrack kTrackExpected[]{
    {IntentKind::SelectAudio, IntentStatus::Ok, 3, kNone}, {IntentKind::SelectSubtitles, IntentStatus::Ok, 3, 7},
    {IntentKind::SelectSubtitles, IntentStatus::Ok, 3, 6}, {IntentKind::SelectAudio, IntentStatus::Ok, 2, 6},
    {IntentKind::SelectAudio, IntentStatus::Ok, 4, 6}, {IntentKind::SelectAudio, IntentStatus::Unavailable, 4, 6},
    {IntentKind::SelectSubtitles, IntentStatus::Ok, 4, kNone}, {IntentKind::SelectAudio, IntentStatus::Ok, 9, kNone},
    {IntentKind::SeekRelative, IntentStatus::Ok, 9, kNone}, {IntentKind::SelectAudio, IntentStatus::Ok, 2, kNone},
    {IntentKind::SelectSubtitles, IntentStatus::Ok, 2, 6}, {IntentKind::SelectSubtitles, IntentStatus::NotFound, 2, 6},
    {IntentKind::Pause, IntentStatus::Ok, 2, 6}, {IntentKind::Play, IntentStatus::Ok, 2, 6}};
constexpr ExpectedTrack kTrackSurroundExpected[]{
    {IntentKind::SelectAudio, IntentStatus::Ok, 2, kNone}, {IntentKind::SelectAudio, IntentStatus::Ok, 4, kNone},
    {IntentKind::SelectAudio, IntentStatus::Ok, 9, kNone}, {IntentKind::SelectAudio, IntentStatus::Ok, 3, kNone}};
// --test-fixture: a real-world file (assets/test_fixtures) of any length, played for three seconds and then closed
// with Escape as a user would: at least 60 pictures and two seconds of audio, nothing dropped, nothing allocated.
constexpr media::engine::ScriptedTyping kFixtureScript[]{{3000, L"\x1b"}};
// --test-pacing (docs/session.md, "Low-power pacing"): play to 6 s, pause for 4 s, play to 14 s and close; the CPU shares of
// the presentation thread, the worker and the process while playing and while paused, and no picture dropped.
constexpr media::engine::ScriptedTyping kPacingScript[]{{14000, L"\x1b"}};
// The most the presentation thread and the worker may each take, as a share of one core (measured 2026-10-22, Release, 60 fps:
// about 0.09 and 0.07 playing, 0.01 paused; both were 0.99 spinning).
constexpr double kPacingPlaying = 0.20, kPacingPaused = 0.05;
// --test-fixture-hdr: the same for a 4K 10-bit PQ or HLG file at 50 fps or more: every picture tone-mapped, and
// playback continuous from the first picture (the last picture shown is as many frame intervals in as pictures were
// shown, so none was skipped at the start or later).
// --test-fixture-search: the same with assets/test_fixtures/big_buck_bunny.srt (Hebrew cues with English, Arabic and
// Chinese lines). Quotes in four scripts are looked for while playing, two as spoken-style commands, one typed into
// the command bar as a bare quote and one as a typed command; each must land on its cue, minutes apart in the file.
// Then two that are in no cue, found by concept search (docs/subtitle-search.md, "Concept search"): "find revenge"
// (said; cue 9, "זמן לנקמה!") and "fruit falling" (typed; cue 4, "תפוח נופל מן העץ"), each with its "Concept match" cue.
// The seeks are 2.5 s apart: a downloaded file has sync samples seconds apart, so a landing decodes up to a few
// hundred pictures first (0.5 s and more), and a newer seek would overtake one still landing.
// (Scripted commands come before the typing: both are counted as results in the order given.)
constexpr media::engine::ScriptedCommand kFixtureQuotes[]{{800, "מצא הארנב ישן"}, {3300, "find something snaps"}, {5800, "find revenge"}};
constexpr media::engine::ScriptedTyping kFixtureSearchScript[]{
    {8300, L"/保护蝴蝶"}, {8450, L"\r"}, {10800, L"/find السناجب"}, {10950, L"\r"}, {13300, L"/fruit falling"}, {13450, L"\r"}, {16000, L"\x1b"}};
// --test-chapters on the chapter fixtures (chapters and hard cuts at 3 s and 6 s of 9 s): chapter and scene commands in
// English and Hebrew while playing, and Page Up (PlaybackOptions::test_chapters: each step runs once what it depends on
// has happened; the times below are only nominal). Each is checked against the cue it shows; live cuts are seen at 3 s
// (playing on from 0) and 6 s (from 3 s), so "next scene" first finds nothing.
constexpr media::engine::ScriptedCommand kChapterScript[]{
    {1000, "next chapter"},
    {2000, "\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\x94\xd7\x91\xd7\x90"},                           // פרק הבא
    {3000, "previous chapter"},
    {4000, "\xd7\x94\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\x94\xd7\xa7\xd7\x95\xd7\x93\xd7\x9d"},        // הפרק הקודם
    {8500, "\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\xa7\xd7\x95\xd7\x93\xd7\x9e\xd7\xaa"},      // סצנה קודמת
    {9500, "next scene"},
    {14000, "next scene"},
};
// The cue of each navigation, in order (the seventh is Page Up, between the last two commands).
constexpr const char* kChapterCues[]{
    "Chapter 2 \xc2\xb7 \xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa9\xd7\xa0\xd7\x99 \xc2\xb7 00:03", "Chapter 3 \xc2\xb7 Finale \xc2\xb7 00:06",
    "Chapter 2 \xc2\xb7 \xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa9\xd7\xa0\xd7\x99 \xc2\xb7 00:03", "Chapter 1 \xc2\xb7 Opening \xc2\xb7 00:00",
    "Scene 1 \xc2\xb7 00:03", "No next scene", "Chapter 2 \xc2\xb7 \xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa9\xd7\xa0\xd7\x99 \xc2\xb7 00:03",
    "Scene 2 \xc2\xb7 00:06"};
constexpr std::int64_t kChapterTargets[]{30000000, 60000000, 30000000, 0, 30000000, -1, 30000000, 60000000};
constexpr unsigned kFixtureCues[]{2, 8, 9, 7, 5, 4}; // Cue indices (26 s, 4:10, 5:00, 3:15, 2:10, 1:23) in the order asked.
constexpr bool kFixtureConcepts[]{false, false, true, false, false, true}; // Found by concept search.
}
// What a due picture waited for (D3D12Renderer::Wait), as words.
const char* Waited(std::uint8_t mask) {
    static const char* const names[16]{" nothing", " its conversion", " the back buffer", " its conversion, the back buffer", " frame latency",
        " its conversion, frame latency", " the back buffer, frame latency", " its conversion, the back buffer, frame latency", " Present",
        " its conversion, Present", " the back buffer, Present", " its conversion, the back buffer, Present", " frame latency, Present",
        " its conversion, frame latency, Present", " the back buffer, frame latency, Present", " its conversion, the back buffer, frame latency, Present"};
    return names[mask & 15];
}
const char* StatusName(media::render::SubtitleStatus status) {
    using media::render::SubtitleStatus;
    return status == SubtitleStatus::Ready ? "ready" : status == SubtitleStatus::AtlasExhausted ? "limits exceeded (some glyphs left out)" :
           status == SubtitleStatus::Disabled ? "disabled" : "off";
}
// --test-voice: the voice script, built from the corpora of voices that are in no template (setup, before playback):
// single words from voice_corpus.bin, spoken sentences from voice_sentences.bin beside it, and the wake word ("avion",
// alone or before a command in one breath) from voice_wake.bin beside it. Nothing said without the name may run.
constexpr media::engine::ScriptedTyping kVoiceEnd[]{{24500, L"\x1b"}}; // After the 24 s script.
constexpr media::engine::ScriptedTyping kQualityEnd[]{{13000, L"\x1b"}}; // After the governor's 11 s script.
bool BuildVoiceScript(const wchar_t* path, std::vector<float>& script) {
    struct Pick { const char* label; double at; };
    const Pick words[]{{"pause | Mark OneCore rate 0 pitch 0", 1.0}, {"pause | Mark OneCore rate 0 pitch 0", 3.6},
                       {"\xd7\xa0\xd7\x92\xd7\x9f | Julie OneCore rate 0 pitch 0", 6.5},
                       {"quieter | Mark OneCore rate 0 pitch 0", 9.5}, {"garden | Mark OneCore rate 0 pitch 0", 11.0}};
    const Pick sentences[]{{"can you please pause the movie | Mark rate 0", 16.0}};
    const Pick wakes[]{{"avion | Mark OneCore rate 0 pitch 0", 2.5}, {"avion | Julie OneCore rate 0 pitch 0", 5.5}, {"avion | Mark OneCore rate 0 pitch 0", 8.5},
                       {"avion turn up the volume a bit | Mark rate -1", 12.5}, {"avion can you please pause the movie | Mark rate -1", 18.0},
                       {"avion next chapter | Mark rate -1", 20.3}}; // The long-form fixture's synthesized sections (docs/chapters.md).
    script.assign(16000 * 24, 0.0f);
    std::uint32_t state = 777; for (auto& s : script) { state = state * 1664525u + 1013904223u; s = 0.0003f * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f); }
    unsigned placed = 0;
    const auto place = [&](const char* label, const std::vector<std::int16_t>& pcm, std::span<const Pick> picks) {
        for (const auto& p : picks) if (std::strcmp(label, p.label) == 0) {
            const auto start = static_cast<std::size_t>(p.at * 16000);
            for (std::size_t k = 0; k < pcm.size() && start + k < script.size(); ++k) script[start + k] += pcm[k] / 32768.0f;
            ++placed;
        }
    };
    // MVC1: count, then per item: command, label bytes, label, samples, int16 samples.
    FILE* f = nullptr; if (_wfopen_s(&f, path, L"rb") || !f) return false;
    char magic[4]{}; std::uint32_t count = 0;
    bool good = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "MVC1", 4) == 0 && std::fread(&count, 4, 1, f) == 1;
    for (std::uint32_t i = 0; good && i < count; ++i) {
        std::uint32_t command = 0, bytes = 0, samples = 0; char label[128]{};
        good = std::fread(&command, 4, 1, f) == 1 && std::fread(&bytes, 4, 1, f) == 1 && bytes < sizeof(label) && std::fread(label, 1, bytes, f) == bytes &&
               std::fread(&samples, 4, 1, f) == 1 && samples < 16000 * 5;
        if (!good) break;
        std::vector<std::int16_t> pcm(samples); good = std::fread(pcm.data(), 2, samples, f) == samples;
        place(label, pcm, words);
    }
    std::fclose(f);
    // MVS1 files beside it: count, then per item: label bytes, label, meaning bytes, meaning, samples, int16 samples.
    std::wstring folder{path}; const auto slash = folder.find_last_of(L"\\/");
    folder = slash == std::wstring::npos ? std::wstring{} : folder.substr(0, slash + 1);
    const auto sentences_file = [&](const wchar_t* name, std::span<const Pick> picks) {
        FILE* g = nullptr; if (_wfopen_s(&g, (folder + name).c_str(), L"rb") || !g) return false;
        bool ok = std::fread(magic, 1, 4, g) == 4 && std::memcmp(magic, "MVS1", 4) == 0 && std::fread(&count, 4, 1, g) == 1;
        for (std::uint32_t i = 0; ok && i < count; ++i) {
            std::uint32_t bytes = 0, means = 0, samples = 0; char label[1024]{}, meaning[512]{};
            ok = std::fread(&bytes, 4, 1, g) == 1 && bytes < sizeof(label) && std::fread(label, 1, bytes, g) == bytes &&
                 std::fread(&means, 4, 1, g) == 1 && means < sizeof(meaning) && std::fread(meaning, 1, means, g) == means &&
                 std::fread(&samples, 4, 1, g) == 1 && samples < 16000 * 9;
            if (!ok) break;
            std::vector<std::int16_t> pcm(samples); ok = std::fread(pcm.data(), 2, samples, g) == samples;
            place(label, pcm, picks);
        }
        std::fclose(g);
        return ok;
    };
    good = good && sentences_file(L"voice_sentences.bin", sentences) && sentences_file(L"voice_wake.bin", wakes);
    return good && placed == std::size(words) + std::size(sentences) + std::size(wakes);
}
// ---- Sessions (docs/session.md) ----
// A folder session: the file asked for, then the next or previous playable file of its folder whenever "next video",
// Ctrl+Right and the like end a file's Run. Each file gets a coordinator of its own, prepared after the previous one
// closed (Stopped -> Preparing: the allocations of a new file happen there, never while one plays); the window is handed
// on; the resume ledger is read when a file opens and saved when it closes. A file that cannot be prepared is passed
// over in the direction of travel. `script`: --test-resume's actions per file opened (nullptr: none).
struct SessionScript {
    const wchar_t* name; // The file this step must open.
    const media::engine::ScriptedCommand* commands; unsigned command_count;
    const media::engine::ScriptedTyping* typing; unsigned typing_count;
};
struct SessionFile {
    wchar_t name[128]{};
    bool adopted = false, known = false, resumed = false, landed = false, restored_audio = false, restored_subtitles = false, recorded = false, music = false;
    std::int64_t target = 0, pts = 0, duration = 0, latency = 0, written = 0, first_frame = 0, gap = 0, prepare = 0, close = 0, save = 0; // 100 ns.
    std::uint32_t audio = 0, subtitles = 0, written_audio = 0, written_subtitles = 0;
    media::engine::PlaybackCoordinator::FileRequest request{};
    unsigned presented = 0, dropped = 0, error = 0, results = 0, music_presents = 0;
    unsigned long long allocations = 0;
    media::engine::PlaybackCoordinator::CommandResult command[4]{};
    char cue[media::ui::StatusCue::kMaxBytes]{};
};
struct SessionRun {
    SessionFile files[8]{}; unsigned opened = 0, skipped = 0; bool saved = true;
    bool warm = true;                        // A warm session (docs/session.md, "Warm switch"); --cold: each file makes its own.
    media::engine::WarmSession::Reuse reuse{}; // What the warm session reused, at its end.
};
int RunSession(const wchar_t* path, media::engine::PlaybackOptions options, const wchar_t* ledger_path, const SessionScript* script, unsigned steps,
               SessionRun& run) {
    using media::engine::PlaybackCoordinator;
    // Made once, at the session's start (the device, window, decoder, endpoint and arena it will keep come with the first
    // file); released when the session ends, after its last coordinator.
    auto warm = run.warm ? std::make_unique<media::engine::WarmSession>() : nullptr;
    struct Ending { std::unique_ptr<media::engine::WarmSession>& warm; SessionRun& run; ~Ending() { if (warm) { run.reuse = warm->Stats(); warm->Close(); } } } ending{warm, run};
    static media::session::ResumeLedger ledger;  // Fixed storage: the ledger and the folder's names.
    static media::session::Playlist playlist;
    static wchar_t folder[4096], current[4096];
    (void)media::platform::windows::LoadLedger(ledger_path, ledger);
    int index = -1;
    const bool listed = media::platform::windows::ListFolder(path, playlist, folder, std::size(folder), index);
    if (!listed || index < 0) { wcscpy_s(current, path); playlist.Clear(); index = -1; } // Alone: no folder to go through.
    else std::swprintf(current, std::size(current), L"%s%s", folder, std::wstring{playlist.At(static_cast<unsigned>(index))}.c_str());
    media::render::WindowHandoff window{};
    int direction = 0;
    const auto* sidecar = options.subtitle_path; // The first file's only.
    std::uint64_t ended = 0; // QueryPerformanceCounter when the previous file's Run returned.
    std::int64_t closing = 0, saving = 0; // The previous file's Close and the ledger's save after it (100 ns).
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    for (;;) {
        auto player = std::make_unique<PlaybackCoordinator>(warm.get());
        auto file_options = options;
        file_options.key_context = player.get(); file_options.resume = &ledger; file_options.window = window;
        file_options.subtitle_path = run.opened == 0 ? sidecar : nullptr;
        if (index >= 0) { file_options.files_before = static_cast<unsigned>(index); file_options.files_after = playlist.Count() - 1u - static_cast<unsigned>(index); }
        if (script) {
            if (run.opened < steps) {
                const auto& s = script[run.opened];
                file_options.commands = s.commands; file_options.command_count = s.command_count;
                file_options.typing = s.typing; file_options.typing_count = s.typing_count;
            } else { file_options.commands = nullptr; file_options.command_count = 0; file_options.typing = nullptr; file_options.typing_count = 0; }
        } else if (run.opened) { file_options.commands = nullptr; file_options.command_count = 0; } // --command: the first file's.
        const wchar_t* name = std::wcsrchr(current, L'\\'); name = name ? name + 1 : current;
        LARGE_INTEGER preparing{}; QueryPerformanceCounter(&preparing);
        if (!player->Prepare(current, file_options)) {
            // Passed over in the direction of travel (or the session ends: the first file, or nothing further that way).
            std::wprintf(L"Session: %ls cannot be played (stage %u), passed over\n", name, player->Error());
            player->KeepWindow(); player->Close();
            if (player->HandedWindow().window) window = player->HandedWindow(); // Else the window was never taken: it is still the one to hand on.
            ++run.skipped;
            const int next = index + direction;
            if (!direction || index < 0 || next < 0 || next >= static_cast<int>(playlist.Count())) return 1;
            index = next;
            std::swprintf(current, std::size(current), L"%s%s", folder, std::wstring{playlist.At(static_cast<unsigned>(index))}.c_str());
            continue;
        }
        auto& f = run.files[run.opened < std::size(run.files) ? run.opened : std::size(run.files) - 1];
        f = {}; (void)wcsncpy_s(f.name, name, _TRUNCATE);
        f.adopted = player->AdoptedWindow();
        LARGE_INTEGER began{}; QueryPerformanceCounter(&began);
        f.prepare = (began.QuadPart - preparing.QuadPart) * 10000000 / frequency.QuadPart; f.close = closing; f.save = saving;
        allocation_probe::Begin();
        const bool played = player->Run();
        f.allocations = allocation_probe::End();
        const auto& r = player->Resume();
        f.known = r.known; f.resumed = r.resumed; f.landed = r.landed; f.target = r.target; f.pts = r.pts; f.duration = r.duration; f.latency = r.latency;
        f.restored_audio = r.restored_audio; f.restored_subtitles = r.restored_subtitles; f.recorded = r.recorded;
        f.written = r.written.position; f.written_audio = r.written.audio_track; f.written_subtitles = r.written.subtitle_track;
        std::memcpy(f.cue, r.cue, sizeof(f.cue));
        f.audio = player->AudioTrackId(); f.subtitles = player->SubtitleTrackId();
        f.request = player->RequestedFile(); f.presented = player->Presented(); f.dropped = player->Dropped(); f.error = player->Error();
        f.music = player->MusicMode(); f.music_presents = player->Renderer().MusicPresents();
        f.first_frame = player->FirstFrameTime();
        f.results = player->CommandResults();
        for (unsigned i = 0; i < f.results && i < std::size(f.command); ++i) f.command[i] = player->CommandAt(i);
        // The pause between files: the previous Run's end to this file's first picture.
        if (ended) f.gap = static_cast<std::int64_t>((static_cast<std::uint64_t>(began.QuadPart) - ended) * 10000000ull / static_cast<std::uint64_t>(frequency.QuadPart)) + f.first_frame;
        ++run.opened;
        std::printf("Session file %u: %ls: %s%s%s frames=%u dropped=%u allocations=%llu first picture %.1f ms%s\n", run.opened, f.name,
                    f.adopted ? "same window, " : "new window, ", f.resumed ? "resumed, " : f.known ? "known, " : "", f.cue, f.presented, f.dropped,
                    f.allocations, f.first_frame / 1e4, played ? "" : " (Run failed)");
        if (f.resumed) std::printf("  resume: target %.3fs landed %s pts %.4fs (+%.1f ms frame), latency %.1f ms\n", f.target / 1e7, f.landed ? "at" : "NOT",
                                   f.pts / 1e7, f.duration / 1e4, f.latency / 1e4);
        if (f.gap) std::printf("  between files: %.1f ms from the previous Run's end to this file's first picture (closing %.1f ms, saving the ledger %.1f ms, preparing %.1f ms)\n",
                               f.gap / 1e4, f.close / 1e4, f.save / 1e4, f.prepare / 1e4);
        {
            const auto& t = player->PrepareTimes();
            std::printf("  prepare: arena %.1f, open %.1f, metadata %.1f, renderer %.1f, subtitles %.1f, chapters %.1f, decoder %.1f, audio %.1f; total %.1f ms\n",
                        t.arena / 1e4, t.open / 1e4, t.metadata / 1e4, t.renderer / 1e4, t.subtitles / 1e4, t.timeline / 1e4, t.decoder / 1e4, t.audio / 1e4, t.total / 1e4);
        }
        const auto request = player->RequestedFile();
        if (request != PlaybackCoordinator::FileRequest::None && !warm) player->KeepWindow(); // A warm session keeps it anyway.
        LARGE_INTEGER returned{}; QueryPerformanceCounter(&returned);
        player->Close();
        LARGE_INTEGER stopped{}; QueryPerformanceCounter(&stopped);
        closing = (stopped.QuadPart - returned.QuadPart) * 10000000 / frequency.QuadPart; ended = static_cast<std::uint64_t>(returned.QuadPart);
        window = player->HandedWindow();
        LARGE_INTEGER saving_from{}; QueryPerformanceCounter(&saving_from);
        run.saved = media::platform::windows::SaveLedger(ledger_path, ledger) && run.saved;
        LARGE_INTEGER saved{}; QueryPerformanceCounter(&saved); saving = (saved.QuadPart - saving_from.QuadPart) * 10000000 / frequency.QuadPart;
        if (request == PlaybackCoordinator::FileRequest::None || !played || index < 0) return played ? 0 : 1;
        direction = request == PlaybackCoordinator::FileRequest::Next ? 1 : -1;
        index += direction;
        if (index < 0 || index >= static_cast<int>(playlist.Count())) return 1; // The coordinator checked: cannot happen.
        std::swprintf(current, std::size(current), L"%s%s", folder, std::wstring{playlist.At(static_cast<unsigned>(index))}.c_str());
    }
}
// --test-resume (the folder built beside the long-form fixture, docs/session.md): Episode 1 (the multi-track fixture),
// Episode 2 (the 3.5-hour long-form fixture), Episode 3 (not a film), Episode 10 (the 3 s fixture) and notes.txt, in a
// fresh ledger. The session goes 2 -> 10 (Ctrl+Right, past 3) -> 2 ("הסרטון הקודם", past 3; resumed at 1:02:0x) -> 1
// ("previous video"; Hebrew audio and English subtitles chosen) -> 2 ("next video"; resumed) -> 1 (Ctrl+Left; the tracks
// restored), and Escape.
constexpr media::engine::ScriptedCommand kResumeSeek[]{{800, "go to 1:02:03"}};
constexpr media::engine::ScriptedTyping kResumeNext[]{{2600, L"\x1d"}};
constexpr media::engine::ScriptedCommand kResumeBack[]{{600, "הסרטון הקודם"}};
constexpr media::engine::ScriptedCommand kResumePrevious[]{{1500, "previous video"}};
constexpr media::engine::ScriptedCommand kResumeTracks[]{{800, "hebrew audio"}, {1300, "english subtitles"}, {2200, "next video"}};
constexpr media::engine::ScriptedTyping kResumeLeft[]{{1200, L"\x1c"}};
constexpr media::engine::ScriptedTyping kResumeEnd[]{{1500, L"\x1b"}};
constexpr SessionScript kResumeScript[]{
    {L"Episode 2.mp4", kResumeSeek, 1, kResumeNext, 1}, {L"Episode 10.mp4", kResumeBack, 1, nullptr, 0},
    {L"Episode 2.mp4", kResumePrevious, 1, nullptr, 0}, {L"Episode 1.mp4", kResumeTracks, 3, nullptr, 0},
    {L"Episode 2.mp4", nullptr, 0, kResumeLeft, 1}, {L"Episode 1.mp4", nullptr, 0, kResumeEnd, 1}};
int ResumeTest(const wchar_t* longform, media::engine::PlaybackOptions options, bool warm) {
    using media::engine::PlaybackCoordinator; using Request = PlaybackCoordinator::FileRequest;
    // The folder, beside the fixtures (setup, before any measurement).
    std::wstring dir{longform}; const auto slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring{L"."} : dir.substr(0, slash);
    const std::wstring multitrack = dir + L"\\playback_multitrack.mp4", clip = dir + L"\\playback_fixture.mp4", folder = dir + L"\\resume_session";
    if (GetFileAttributesW(multitrack.c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(clip.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::puts("SKIP fixture not generated"); return 77;
    }
    (void)CreateDirectoryW(folder.c_str(), nullptr);
    const auto place = [&](const wchar_t* name, const std::wstring& from) {
        const auto to = folder + L"\\" + name; (void)DeleteFileW(to.c_str());
        return CreateHardLinkW(to.c_str(), from.c_str(), nullptr) || CopyFileW(from.c_str(), to.c_str(), FALSE);
    };
    const auto write = [&](const wchar_t* name, const char* text) {
        const auto to = folder + L"\\" + name; FILE* f = nullptr;
        if (_wfopen_s(&f, to.c_str(), L"wb") || !f) return false;
        const bool ok = std::fwrite(text, 1, std::strlen(text), f) == std::strlen(text); std::fclose(f); return ok;
    };
    const auto ledger_path = folder + L"\\resume.bin";
    (void)DeleteFileW(ledger_path.c_str());
    if (!place(L"Episode 1.mp4", multitrack) || !place(L"Episode 2.mp4", longform) || !place(L"Episode 10.mp4", clip) ||
        !write(L"Episode 3.mp4", "Not a film: the session must pass over it.\n") || !write(L"notes.txt", "Not playable.\n")) {
        std::puts("Cannot build the session folder"); return 1;
    }
    static SessionRun run; run.warm = warm;
    const int code = RunSession((folder + L"\\Episode 2.mp4").c_str(), options, ledger_path.c_str(), kResumeScript, static_cast<unsigned>(std::size(kResumeScript)), run);
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL %s\n", what); } };
    const auto& reuse = run.reuse;
    if (warm) std::printf("Warm session: %u files prepared; kept: arena %u, device and window %u, slots %u, decoder %u, endpoint %u; made: decoder %u, endpoint %u\n",
                          reuse.files, reuse.arenas_kept, reuse.gpu_kept, reuse.slots_kept, reuse.decoders_kept, reuse.endpoints_kept, reuse.decoders_made,
                          reuse.endpoints_made);
    check(code == 0, "the session ended with Escape");
    check(run.opened == std::size(kResumeScript), "six files opened");
    check(run.skipped == 2, "Episode 3 passed over twice");
    check(run.saved, "the ledger saved after every file");
    const auto& f = run.files;
    for (unsigned i = 0; i < run.opened && i < std::size(kResumeScript); ++i) {
        char what[96]; std::snprintf(what, sizeof(what), "file %u is %ls", i + 1, kResumeScript[i].name);
        check(std::wcscmp(f[i].name, kResumeScript[i].name) == 0, what);
        std::snprintf(what, sizeof(what), "file %u: no allocation, no error, no drop, pictures shown", i + 1);
        check(f[i].allocations == 0 && f[i].error == 0 && f[i].dropped == 0 && f[i].presented > 0, what);
        std::snprintf(what, sizeof(what), "file %u: %s window", i + 1, i ? "the same" : "a new");
        check(f[i].adopted == (i != 0), what);
    }
    if (run.opened == std::size(kResumeScript)) {
        constexpr std::int64_t kS = 10000000, kSeek = 3723 * kS;
        check(!f[0].known && !f[0].resumed && f[0].request == Request::Next, "1: new to the ledger, Ctrl+Right");
        check(f[0].results == 1 && f[0].command[0].intent.kind == IntentKind::SeekAbsolute && f[0].command[0].landed, "1: the seek to 1:02:03 landed");
        check(f[0].recorded && f[0].written >= kSeek && f[0].written < kSeek + 4 * kS, "1: left at 1:02:03 and a little more");
        check(!f[1].known && f[1].results == 1 && f[1].command[0].intent.kind == IntentKind::PreviousFile && f[1].request == Request::Previous,
              "2: \"הסרטון הקודם\" asks for the previous file");
        check(f[1].recorded && f[1].written < 15 * kS, "2: left within its first seconds");
        // Resumed where it was left: the landing picture holds the position recorded.
        check(f[2].known && f[2].resumed && f[2].target == f[0].written && f[2].landed && f[2].pts <= f[2].target && f[2].target < f[2].pts + f[2].duration,
              "3: resumed on the picture at the position left");
        check(std::strstr(f[2].cue, "from 1:02:0") != nullptr && std::strstr(f[2].cue, " of 4 ") != nullptr, "3: the cue names the place and the time");
        check(f[2].results == 1 && f[2].command[0].intent.kind == IntentKind::PreviousFile && f[2].request == Request::Previous, "3: \"previous video\"");
        check(!f[3].known && f[3].results == 3 && f[3].command[0].intent.kind == IntentKind::SelectAudio && f[3].command[1].intent.kind == IntentKind::SelectSubtitles &&
              f[3].command[2].intent.kind == IntentKind::NextFile && f[3].request == Request::Next, "4: tracks chosen, then \"next video\"");
        check(f[3].written_audio == 3 && f[3].written_subtitles == 7, "4: Hebrew audio (3) and English subtitles (7) recorded");
        check(f[4].known && f[4].resumed && f[4].target == f[2].written && f[4].landed && f[4].pts <= f[4].target && f[4].target < f[4].pts + f[4].duration &&
              f[4].request == Request::Previous, "5: resumed again, then Ctrl+Left");
        check(f[5].known && !f[5].resumed && f[5].restored_audio && f[5].restored_subtitles && f[5].audio == 3 && f[5].subtitles == 7 &&
              f[5].request == Request::None, "6: its tracks as they were left, from the start (under 15 s in)");
    }
    // Warm switch (docs/session.md, "Warm switch"): from one file's end to the next one's first picture, under 50 ms for
    // the ordinary files (the 3.5-hour one validates its sample tables first, then lands a resume seek). The second
    // file's includes passing over Episode 3.
    if (warm && run.opened == std::size(kResumeScript))
        for (const unsigned i : {1u, 3u, 5u}) {
            char what[96]; std::snprintf(what, sizeof(what), "file %u: warm switch in %.1f ms, under 50 ms", i + 1, f[i].gap / 1e4);
            std::printf("Warm switch to file %u: %.1f ms\n", i + 1, f[i].gap / 1e4);
            check(f[i].gap > 0 && f[i].gap < 500000, what);
        }
    // The ledger on disk: the three files, as they were left.
    media::session::ResumeLedger saved;
    check(media::platform::windows::LoadLedger(ledger_path.c_str(), saved) && saved.Count() == 3, "the saved ledger holds three files");
    std::uint64_t key = 0, bytes = 0;
    if (media::platform::windows::FileIdentity((folder + L"\\Episode 2.mp4").c_str(), key, bytes)) {
        const auto* e = saved.Find(key, bytes);
        check(e && e->position == f[4].written && media::session::ResumeLedger::ResumeAt(*e) == e->position, "Episode 2 resumes from where it was left last");
    }
    if (media::platform::windows::FileIdentity((folder + L"\\Episode 1.mp4").c_str(), key, bytes)) {
        const auto* e = saved.Find(key, bytes);
        check(e && e->audio_track == 3 && e->subtitle_track == 7 && media::session::ResumeLedger::ResumeAt(*e) == 0, "Episode 1: its tracks, from the start");
    }
    std::printf("Resume session: %u files opened, %u passed over: %s (%d failures)\n", run.opened, run.skipped, failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
// --test-music-session (docs/session.md, "Warm switch"; docs/audio-dsp.md, "Music mode"): a folder beside the fixtures of
// the 3 s fixture, the M4A and the 3 s fixture again, in a fresh ledger: Ctrl+Right, "next video", Escape. Music mode comes
// and goes in one warm session: one window throughout, the video decoder kept across the music file, each switch under
// 50 ms, nothing allocated in any file.
constexpr media::engine::ScriptedTyping kMusicNext[]{{1000, L"\x1d"}};
constexpr media::engine::ScriptedCommand kMusicNextVideo[]{{1500, "next video"}};
constexpr media::engine::ScriptedTyping kMusicEnd[]{{1000, L"\x1b"}};
constexpr SessionScript kMusicScript[]{
    {L"Track 1.mp4", nullptr, 0, kMusicNext, 1}, {L"Track 2.m4a", kMusicNextVideo, 1, nullptr, 0}, {L"Track 3.mp4", nullptr, 0, kMusicEnd, 1}};
int MusicSessionTest(const wchar_t* music, media::engine::PlaybackOptions options, bool warm) {
    using media::engine::PlaybackCoordinator; using Request = PlaybackCoordinator::FileRequest;
    std::wstring dir{music}; const auto slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring{L"."} : dir.substr(0, slash);
    const std::wstring clip = dir + L"\\playback_fixture.mp4", folder = dir + L"\\music_session";
    if (GetFileAttributesW(clip.c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(music) == INVALID_FILE_ATTRIBUTES) { std::puts("SKIP fixture not generated"); return 77; }
    (void)CreateDirectoryW(folder.c_str(), nullptr);
    const auto place = [&](const wchar_t* name, const std::wstring& from) {
        const auto to = folder + L"\\" + name; (void)DeleteFileW(to.c_str());
        return CreateHardLinkW(to.c_str(), from.c_str(), nullptr) || CopyFileW(from.c_str(), to.c_str(), FALSE);
    };
    const auto ledger_path = folder + L"\\resume.bin";
    (void)DeleteFileW(ledger_path.c_str());
    if (!place(L"Track 1.mp4", clip) || !place(L"Track 2.m4a", music) || !place(L"Track 3.mp4", clip)) { std::puts("Cannot build the session folder"); return 1; }
    static SessionRun run; run.warm = warm;
    const int code = RunSession((folder + L"\\Track 1.mp4").c_str(), options, ledger_path.c_str(), kMusicScript, static_cast<unsigned>(std::size(kMusicScript)), run);
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL %s\n", what); } };
    const auto& reuse = run.reuse; const auto& f = run.files;
    if (warm) std::printf("Warm session: %u files prepared; kept: arena %u, device and window %u, slots %u, decoder %u, endpoint %u; made: decoder %u, endpoint %u\n",
                          reuse.files, reuse.arenas_kept, reuse.gpu_kept, reuse.slots_kept, reuse.decoders_kept, reuse.endpoints_kept, reuse.decoders_made,
                          reuse.endpoints_made);
    check(code == 0 && run.opened == 3 && run.skipped == 0, "three files opened, the session ended with Escape");
    for (unsigned i = 0; i < run.opened && i < 3; ++i) {
        char what[96]; std::snprintf(what, sizeof(what), "file %u is %ls, %s window, no allocation, error or drop", i + 1, kMusicScript[i].name, i ? "the same" : "a new");
        check(std::wcscmp(f[i].name, kMusicScript[i].name) == 0 && f[i].adopted == (i != 0) && f[i].allocations == 0 && f[i].error == 0 && f[i].dropped == 0, what);
    }
    if (run.opened == 3) {
        check(!f[0].music && f[0].presented > 0 && f[0].request == Request::Next, "1: pictures, then Ctrl+Right");
        check(f[1].music && f[1].presented == 0 && f[1].music_presents > 30 && f[1].results == 1 && f[1].command[0].intent.kind == IntentKind::NextFile &&
              f[1].request == Request::Next, "2: music mode, its sound drawn, then \"next video\"");
        check(!f[2].music && f[2].presented > 0 && f[2].request == Request::None, "3: pictures again, then Escape");
        if (warm) {
            // The decoder and the RGB slots made for the first file serve the third: the music file left them alone.
            check(reuse.decoders_made == 1 && reuse.decoders_kept == 1 && reuse.slots_kept == 1 && reuse.gpu_kept == 2 && reuse.endpoints_made == 1 &&
                  reuse.endpoints_kept == 2, "one device, decoder, set of slots and endpoint for the three files");
            for (const unsigned i : {1u, 2u}) {
                char what[96]; std::snprintf(what, sizeof(what), "file %u: warm switch in %.1f ms, under 50 ms", i + 1, f[i].gap / 1e4);
                std::printf("Warm switch to file %u (%s): %.1f ms\n", i + 1, f[i].music ? "music" : "pictures", f[i].gap / 1e4);
                check(f[i].gap > 0 && f[i].gap < 500000, what);
            }
        }
    }
    std::printf("Music session: %u files opened: %s (%d failures)\n", run.opened, failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
int wmain(int argc, wchar_t** argv) {
    // Physical pixels and WM_DPICHANGED on every monitor (the renderer's window also requests this itself).
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2 || argc > 10) {
        std::puts("Usage: media_player_demo file.mp4 [--test | --test-trim | --test-underrun | --test-pause | --test-hdr | --test-hud | --test-seek | --test-scrub | --test-dsp | --test-subtitles | --test-subtitle-scrub | --test-subtitle-overflow | --test-subtitle-missing | --test-resize | --test-surround | --test-surround71 | --test-neural | --test-longform | --test-device-loss | --test-device-loss-paused | --test-audio-loss | --test-volume | --test-intents | --test-palette | --test-fixture | --test-fixture-search | --test-fixture-hdr | --test-anamorphic | --test-voice corpus.bin | --expect-stage N] [--http | --http-slow]\n"
                  "file.mp4 may also be an http:// or https:// URL. --http serves the file from a loopback HTTP range server and plays that\n"
                  "URL; --http-slow adds 15 ms per response and a 50 Mbit/s cap.\n"
                  "H.264 progressive I/P or HEVC Main/Main10 (SDR, PQ, HLG) + optional AAC-LC.\n"
                  "Add --subtitles captions.srt (or .vtt) for native multilingual GPU subtitles; --test-subtitles runs the regression.\n"
                  "Space or the centre button pauses/resumes; drag or click the timeline to seek. Tab/Enter selects controls; arrows seek 5 seconds.\n"
                  "Home/End seeks to the ends; F11, F or Alt+Enter toggles fullscreen; Escape closes. Drag the top strip to move, the edges to resize.\n"
                  "D cycles dialogue clarity (off/light/medium/strong); S toggles binaural headphone audio (initially off; on when 5.1 is folded to stereo).\n"
                  "--surround auto|passthrough|stereo: 5.1 to a surround endpoint as is, or folded to stereo (binaural or ITU downmix).\n"
                  "N toggles neural 2x luma reconstruction while upscaling (--neural starts with it on).\n"
                  "Up/Down or the mouse wheel change the volume in 5 % steps, M mutes; the dock shows a volume pill that can be dragged.\n"
                  "--command \"text\" gives one spoken-style command a second into playback, in English or Hebrew: \"volume 40\", \"mute\",\n"
                  "\"rewind 30s\", \"go to 1:30\", \"pause\", \"find some words\" (jumps to the subtitle line containing them).\n"
                  "/ (or Ctrl+F, Ctrl+K) opens the command bar in the window: type a command in English, Hebrew, Arabic, Chinese, Japanese,\n"
                  "Spanish, French, German or Russian and press Enter; any other text is looked for in the subtitles. Escape dismisses it.\n"
                  "Ctrl+Right / Ctrl+Left (or \"next video\", \"הסרטון הקודם\") play the next or previous file of the folder; a file\n"
                  "opened again resumes where it was left (more than 15 s in and 60 s before the end), with its tracks.");
        return 2;
    }
    // Loopback HTTP: declared before the player so it outlives the player's HTTP source.
    static loopback::Server server;
    std::vector<std::byte> served;
    wchar_t url[512]{};
    bool http = false, slow = false;
    Mode mode = Mode::Play;
    const wchar_t* subtitle_path = nullptr;
    auto surround = media::engine::SurroundOutput::Auto;
    bool neural = false;
    bool queue_seek = true; // --no-queue-seek: every forward seek decodes again (A/B measurements).
    bool scrub_preview = true; // --no-scrub-preview: every scrub seek lands exactly (A/B measurements).
    bool expect_previews = false; // --expect-previews: a scrub test must show keyframe previews while dragging.
    bool cold = false; // --cold: a session whose files make and release their own device, window, decoder and endpoint (A/B).
    bool isolate = true; // --allow-desktop-input: a test window takes the desktop's input (the isolation's negative control).
    bool busy_spin = false; // --busy-spin: the presentation thread spins every pass (low-power pacing off; A/B measurements).
    bool window_thread_off = false; // --window-thread-off: the window on the presentation thread, as before 2026-10-28 (A/B).
    bool no_perception = false;     // --no-perception: no warm perception grids (playback alone, for the benchmark's A/B).
    unsigned pace_spin_us = media::engine::PlaybackOptions{}.pace_spin_us; // --pace-spin-us N: the spin before a deadline (A/B).
    auto voice = media::engine::VoiceInput::Off;
    const wchar_t* voice_corpus = nullptr;
    static std::vector<float> voice_script; // --test-voice: built before playback.
    static char command_text[1024]{}; // --command, as UTF-8.
    static media::engine::ScriptedCommand command{1000, command_text};
    bool commanded = false;
    for (int i = 2; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--http") == 0) http = true;
        else if (std::wcscmp(argv[i], L"--http-slow") == 0) http = slow = true;
        else if (std::wcscmp(argv[i], L"--subtitles") == 0 && i + 1 < argc) subtitle_path = argv[++i];
        else if (std::wcscmp(argv[i], L"--neural") == 0) neural = true;
        else if (std::wcscmp(argv[i], L"--no-queue-seek") == 0) queue_seek = false;
        else if (std::wcscmp(argv[i], L"--no-scrub-preview") == 0) scrub_preview = false;
        else if (std::wcscmp(argv[i], L"--allow-desktop-input") == 0) isolate = false;
        else if (std::wcscmp(argv[i], L"--cold") == 0) cold = true;
        else if (std::wcscmp(argv[i], L"--busy-spin") == 0) busy_spin = true;
        else if (std::wcscmp(argv[i], L"--window-thread-off") == 0) window_thread_off = true;
        else if (std::wcscmp(argv[i], L"--no-perception") == 0) no_perception = true;
        else if (std::wcscmp(argv[i], L"--pace-spin-us") == 0 && i + 1 < argc) pace_spin_us = static_cast<unsigned>(std::wcstoul(argv[++i], nullptr, 10));
        else if (std::wcscmp(argv[i], L"--expect-previews") == 0) expect_previews = true;
        else if (std::wcscmp(argv[i], L"--voice") == 0 && i + 1 < argc) {
            // Opt-in voice commands from a real device: "microphone" or "loopback" (what the speakers play).
            const wchar_t* value = argv[++i];
            voice = std::wcscmp(value, L"microphone") == 0 ? media::engine::VoiceInput::Microphone :
                    std::wcscmp(value, L"loopback") == 0 ? media::engine::VoiceInput::Loopback : media::engine::VoiceInput::Off;
        }
        else if (std::wcscmp(argv[i], L"--test-voice") == 0 && i + 1 < argc) { mode = Mode::Voice; voice_corpus = argv[++i]; }
        else if (std::wcscmp(argv[i], L"--command") == 0 && i + 1 < argc) {
            commanded = WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, command_text, static_cast<int>(sizeof(command_text)), nullptr, nullptr) > 0;
            if (!commanded) return 2;
        }
        else if (std::wcscmp(argv[i], L"--surround") == 0 && i + 1 < argc) {
            const wchar_t* value = argv[++i];
            surround = std::wcscmp(value, L"passthrough") == 0 ? media::engine::SurroundOutput::Passthrough :
                       std::wcscmp(value, L"stereo") == 0 ? media::engine::SurroundOutput::Stereo : media::engine::SurroundOutput::Auto;
        }
        else if (std::wcscmp(argv[i], L"--expect-stage") == 0 && argc == 4) break;
        else { const auto parsed = Parse(argv[i]); if (parsed == Mode::Play || mode != Mode::Play) return 2; mode = parsed; }
    }
    media::engine::PlaybackCoordinator player;
    media::engine::PlaybackOptions options;
    static media::session::ResumeLedger bookmark_ledger; // --test-bookmarks: where the bookmarks are kept between the two openings.
    options.key_handler = AudioKey; options.key_context = &player;
    options.subtitle_path = subtitle_path;
    options.surround = surround;
    options.neural = neural;
    options.queue_seek = queue_seek;
    options.scrub_preview = scrub_preview;
    options.voice = voice;
    options.low_power_pacing = !busy_spin;
    options.message_thread = !window_thread_off;
    options.warm_perception = !no_perception;
    options.pace_spin_us = pace_spin_us;
    if (commanded) { options.commands = &command; options.command_count = 1; }
    // --expect-stage N: preparation must fail deterministically at stage N.
    if (argc == 4 && std::wcscmp(argv[2], L"--expect-stage") == 0) {
        const auto expected = static_cast<unsigned>(std::wcstoul(argv[3], nullptr, 10));
        if (GetFileAttributesW(argv[1]) == INVALID_FILE_ATTRIBUTES) { std::puts("SKIP fixture not generated"); return 77; }
        const bool prepared = player.Prepare(argv[1], options);
        std::printf("Preparation %s: stage=%u native=%08lx (expected stage %u)\n", prepared ? "succeeded" : "failed", player.Error(),
                    static_cast<unsigned long>(player.NativeError()), expected);
        const bool matched = !prepared && player.Error() == expected;
        player.Close(); return matched ? 0 : 1;
    }
    const bool test = mode != Mode::Play;
    if (test) {
        // Fixtures that need a platform encoder may legitimately be absent: CTest skip code.
        if (GetFileAttributesW(argv[1]) == INVALID_FILE_ATTRIBUTES) { std::puts("SKIP fixture not generated"); return 77; }
        // Stray desktop input must not steer a test: the window never takes focus, and no physical input reaches it.
        options.maximum_seconds = 15; options.activate = false; options.isolate_input = isolate;
        options.test_isolation = mode == Mode::Isolation;
        // Most tests script the paused player by polling on its passes; the pacing and fixture runs' scripts are deadlines the
        // coordinator knows (typing, the scripted pause), so they keep the settled pause's long sleep and measure it.
        options.settled_sleep = mode == Mode::Pacing || mode == Mode::Fixture || mode == Mode::FixtureHdr;
        if (mode == Mode::Chapters) {
            options.commands = kChapterScript; options.command_count = static_cast<unsigned>(std::size(kChapterScript));
            options.test_chapters = true; options.maximum_seconds = 25;
        }
        // Underrun: withhold PCM long enough to drain the 16384-frame ring and the endpoint buffer.
        if (mode == Mode::Underrun) { options.stall_audio_at_ms = 1000; options.stall_audio_ms = 600; }
        if (mode == Mode::Hiccup) { options.test_hiccup = true; options.maximum_seconds = 20; }
        if (mode == Mode::Pause) { options.pause_at_ms = 1000; options.pause_ms = 700; }
        options.test_hud = mode == Mode::Hud; options.test_seek = mode == Mode::Seek || mode == Mode::Dsp || mode == Mode::Subtitles;
        options.test_seek_forward = mode == Mode::SeekForward;
        options.test_seek_micro = mode == Mode::SeekMicro;
        options.test_scrub = mode == Mode::Scrub || mode == Mode::SubtitleScrub;
        options.test_longform = mode == Mode::LongForm;
        if (mode == Mode::LongForm) options.maximum_seconds = 40; // Four seeks, then the last seconds of a 3.5-hour file.
        options.test_remote_seek = mode == Mode::RemoteSeek;
        if (mode == Mode::RemoteSeek) options.maximum_seconds = 45; // Five seeks, then the last 4.7 s of a 10-minute file.
        options.test_subtitles = mode == Mode::Subtitles || mode == Mode::SubtitleScrub || mode == Mode::Resize || mode == Mode::DeviceLoss;
        if (mode == Mode::DeviceLossPaused) { options.pause_at_ms = 1000; options.pause_ms = 1500; }
        options.test_subtitle_overflow = mode == Mode::SubtitleOverflow;
        options.test_resize = mode == Mode::Resize;
        options.test_surround = mode == Mode::Surround;
        options.test_neural = mode == Mode::Neural;
        if (mode == Mode::Quality) {
            options.test_quality = true; options.maximum_seconds = 20;
            options.typing = kQualityEnd; options.typing_count = static_cast<unsigned>(std::size(kQualityEnd));
        }
        // A sidecar that does not exist: subtitles must be disabled, never the playback.
        if (mode == Mode::SubtitleMissing) options.subtitle_path = L"media-engine-missing-captions.srt";
        options.test_dsp = mode == Mode::Dsp;
        options.test_volume = mode == Mode::Volume;
        if (mode == Mode::NeuralReuse) { options.test_neural_reuse = true; options.maximum_seconds = 25; }
        if (mode == Mode::Speed) { options.test_speed = true; options.maximum_seconds = 25; }
        if (mode == Mode::Zoom) { options.test_zoom = true; options.maximum_seconds = 20; }
        if (mode == Mode::Rotation) { options.test_rotation = true; options.maximum_seconds = 20; }
        if (mode == Mode::WindowStall) { options.test_window_stall = true; options.maximum_seconds = 10; }
        if (mode == Mode::AutoHorizon || mode == Mode::CropBars) { options.test_auto_horizon = true; options.test_crop_bars = mode == Mode::CropBars; options.maximum_seconds = 15; }
        if (mode == Mode::ZoomLive) { options.test_zoom_live = true; options.maximum_seconds = 25; }
        if (mode == Mode::Tracking) options.test_tracking = true;
        if (mode == Mode::NextTarget) { options.test_next_target = true; options.snapshot_folder = "."; } // S writes beside the fixture's run.
        if (mode == Mode::Vibrometry || mode == Mode::VibrometryChroma) { options.test_vibrometry = true; options.maximum_seconds = 20; } // A failure cap (the file is 12 s).
        if (mode == Mode::Stabilize) { options.test_stabilize = true; options.maximum_seconds = 16; } // A failure cap (the file is 8 s).
        if (mode == Mode::Saccade) options.test_saccade = true;
        if (mode == Mode::AbLoop) { options.test_ab_loop = true; options.test_subtitles = true; options.maximum_seconds = 25; }
        if (mode == Mode::Telemetry) { options.test_telemetry = true; options.maximum_seconds = 28; }
        if (mode == Mode::Bookmarks) { options.test_bookmarks = true; options.maximum_seconds = 30; options.resume = &bookmark_ledger; }
        if (mode == Mode::Music) { options.test_music = true; options.maximum_seconds = 30; }
        if (mode == Mode::PausedQuality) { options.test_paused_quality = true; options.neural = true; options.maximum_seconds = 20; }
        if (mode == Mode::FollowUps) {
            options.test_subtitles = true; options.commands = kFollowUpScript; options.command_count = static_cast<unsigned>(std::size(kFollowUpScript));
        }
        if (mode == Mode::Intents) {
            options.test_subtitles = true; options.commands = kIntentScript; options.command_count = static_cast<unsigned>(std::size(kIntentScript));
        }
        if (mode == Mode::Palette) {
            options.test_subtitles = true; options.typing = kPaletteScript; options.typing_count = static_cast<unsigned>(std::size(kPaletteScript));
        }
        if (mode == Mode::Tracks) {
            options.commands = kTrackScript; options.command_count = static_cast<unsigned>(std::size(kTrackScript));
            options.test_tracks = true; options.maximum_seconds = 30;
        }
        if (mode == Mode::TracksSurround) {
            options.commands = kTrackSurroundScript; options.command_count = static_cast<unsigned>(std::size(kTrackSurroundScript));
            options.test_tracks = true; options.maximum_seconds = 30; options.audio_language = "ara";
        }
        if (mode == Mode::Fixture || mode == Mode::FixtureSearch || mode == Mode::FixtureHdr) options.maximum_seconds = 30;
        if (mode == Mode::Fixture || mode == Mode::FixtureHdr) { options.typing = kFixtureScript; options.typing_count = static_cast<unsigned>(std::size(kFixtureScript)); }
        if (mode == Mode::Attach) options.maximum_seconds = 30; // Subtitles after open (API 17): the attaches run from their own thread below.
        if (mode == Mode::Pacing) {
            options.typing = kPacingScript; options.typing_count = static_cast<unsigned>(std::size(kPacingScript));
            options.pause_at_ms = 6000; options.pause_ms = 4000; options.maximum_seconds = 30;
        }
        if (mode == Mode::Voice) {
            // Voices that are in no template (tools/make_voice_templates.cpp), said into the voice path in real time.
            if (!voice_corpus || !BuildVoiceScript(voice_corpus, voice_script)) { std::puts("SKIP voice corpus not readable"); return 77; }
            options.voice = media::engine::VoiceInput::Script;
            options.voice_script = voice_script.data(); options.voice_script_frames = static_cast<std::uint32_t>(voice_script.size());
            options.typing = kVoiceEnd; options.typing_count = static_cast<unsigned>(std::size(kVoiceEnd)); options.maximum_seconds = 28;
        }
        if (mode == Mode::FixtureSearch) {
            if (!subtitle_path || GetFileAttributesW(subtitle_path) == INVALID_FILE_ATTRIBUTES) { std::puts("SKIP fixture not generated"); return 77; }
            options.commands = kFixtureQuotes; options.command_count = static_cast<unsigned>(std::size(kFixtureQuotes));
            options.typing = kFixtureSearchScript; options.typing_count = static_cast<unsigned>(std::size(kFixtureSearchScript));
        }
        if (options.test_dsp) { AudioKey(&player, 'D'); AudioKey(&player, 'D'); AudioKey(&player, 'D'); player.SetSpatialAudio(true); } // Shift+S's toggle, before any window message.
    }
    // A folder session (docs/session.md): --test-resume, and plain playback of a local file.
    if (mode == Mode::Resume) return ResumeTest(argv[1], options, !cold);
    if (mode == Mode::MusicSession) return MusicSessionTest(argv[1], options, !cold);
    if (mode == Mode::Play && !http) {
        static wchar_t ledger[4096];
        if (!media::platform::windows::DefaultLedgerPath(ledger, std::size(ledger))) ledger[0] = 0;
        static SessionRun run; run.warm = !cold;
        return RunSession(argv[1], options, ledger, nullptr, 0, run);
    }
    const wchar_t* source = argv[1];
    if (http) {
        // Setup, before measurement: the file is served from memory by the allocation-free loopback server.
        FILE* file = nullptr;
        if (_wfopen_s(&file, argv[1], L"rb") || !file) { std::puts("Cannot read the file to serve"); return 1; }
        std::fseek(file, 0, SEEK_END); served.resize(static_cast<std::size_t>(_ftelli64(file))); std::fseek(file, 0, SEEK_SET);
        const bool read = std::fread(served.data(), 1, served.size(), file) == served.size(); std::fclose(file);
        if (!read || !server.Start(served.data(), served.size())) { std::puts("Loopback server failed"); return 1; }
        if (slow) { server.latency_ms = 15; server.bytes_per_ms = 6250; }
        if (mode == Mode::RemoteSeek) {
            // As a Drive or S3 request needs its token: every GET without these headers is answered 401.
            server.require_auth = true;
            options.http_headers = L"Authorization: Bearer loopback-token\r\nX-Avion-Test: mkv-seek";
        }
        // Small fixtures would fit one default 8 MiB chunk: chunks of about half the file force several ranged
        // GETs, read-ahead and chunk overlap, while still exceeding the largest sample (a 4K IRAP picture).
        std::uint32_t chunk = 64 * 1024;
        while (chunk < served.size() / 2 && chunk < 8u * 1024 * 1024) chunk *= 2;
        options.network_chunk_bytes = chunk;
        const wchar_t* name = std::wcsrchr(argv[1], L'\\'); name = name ? name + 1 : argv[1];
        std::swprintf(url, std::size(url), L"http://127.0.0.1:%u/%s", server.Port(), name);
        source = url;
    }
    if (!player.Prepare(source, options)) {
        std::printf("Preparation failed: stage=%u native=%08lx\n", player.Error(), static_cast<unsigned long>(player.NativeError())); return 1;
    }
    if (player.MusicMode()) std::printf("Stream: no picture track: music mode (AAC alone)\n");
    else std::printf("Stream: %s %ux%u %s, reorder depth %u (%s)\n", player.SoftwareVideo() ? "AV1 (the platform's software decoder)" : player.Hevc() ? "HEVC" : "H.264",
                     player.Width(), player.Height(), player.TenBit() ? "10-bit P010" : "8-bit NV12", player.ReorderDepth(),
                     player.Container() == media::demux::ContainerFormat::Matroska ? "Matroska" : "ISO BMFF");
    if (!player.MusicMode()) {
        // The picture as described by the stream and the container, and as shown.
        using namespace media::video;
        const auto& p = player.Picture();
        const auto said = [](const ColorDescription& c, char* text, std::size_t size) {
            if (!c.described && !c.range_described) std::snprintf(text, size, "nothing");
            else std::snprintf(text, size, "%u/%u/%u%s", c.primaries, c.transfer, c.matrix, !c.range_described ? "" : c.full_range ? " full" : " limited");
        };
        char stream[48], container[48]; said(p.bitstream_color, stream, sizeof(stream)); said(p.container_color, container, sizeof(container));
        std::printf("Picture: crop (%u,%u)-(%u,%u) = %ux%u of %ux%u coded; sample aspect %u:%u (%s; stream %u:%u, container %u:%u), shown as %ux%u\n",
                    p.left, p.top, p.right, p.bottom, p.right - p.left, p.bottom - p.top, p.coded_width, p.coded_height, p.sample_aspect.width, p.sample_aspect.height,
                    p.aspect_source == AspectSource::Container ? "container" : p.aspect_source == AspectSource::Bitstream ? "stream" : "square",
                    p.bitstream_aspect.width, p.bitstream_aspect.height, p.container_aspect.width, p.container_aspect.height, p.display.width, p.display.height);
        std::printf("Colour: primaries %s, transfer %s, matrix %s, %s range (stream says %s, container says %s)\n", PrimariesName(p.color.primaries),
                    TransferName(p.color.transfer), MatrixName(p.color.matrix), p.color.full_range ? "full" : "limited", stream, container);
    }
    // Recovery tests: user audio settings that must survive the fault (dialogue clarity medium, binaural on).
    const bool audio_loss = mode == Mode::AudioLoss || mode == Mode::AudioSteal;
    const bool fault_mode = mode == Mode::DeviceLoss || mode == Mode::DeviceLossPaused || audio_loss;
    if (fault_mode) { player.SetClarityLevel(2); player.SetSpatialAudio(true); }
    Fault fault;
    if (mode == Mode::DeviceLoss) { fault.kind = Fault::Kind::Gpu; fault.at_ms = 1500; }
    if (mode == Mode::DeviceLossPaused) { fault.kind = Fault::Kind::Gpu; fault.at_ms = 1700; } // 0.7 s into the pause.
    if (mode == Mode::AudioLoss) { fault.kind = Fault::Kind::AudioInjected; fault.at_ms = 800; fault.hold_ms = 500; }
    if (mode == Mode::AudioSteal) { fault.kind = Fault::Kind::Audio; fault.at_ms = 800; fault.hold_ms = 500; }
    if (mode == Mode::Isolation) { fault.kind = Fault::Kind::Desktop; fault.at_ms = 1000; }
    fault.device = player.Renderer().Device(); fault.player = &player; fault.window = player.Renderer().Window();
    std::thread injector; // Started before measurement: creating a thread allocates.
    if (fault.kind != Fault::Kind::None) {
        injector = std::thread(Inject, std::ref(fault));
        while (!fault.running.load()) Sleep(1); // Its start-up allocates too (per-thread CRT data).
    }
    // Automated runs: a watchdog (docs/session.md, "Watchdog") turns a hang into a quick, explained failure. Either thread
    // making no progress for 20 s, or the run outliving its limit by 15 s, prints where each thread was and ends the process
    // (exit code 3), well inside CTest's timeout. Started before measurement: creating a thread allocates.
    std::atomic<bool> watchdog_stop{false}, watchdog_running{false};
    std::thread watchdog;
    if (test) {
        watchdog = std::thread([&player, &watchdog_stop, &watchdog_running, limit = options.maximum_seconds] {
            const auto started = GetTickCount64();
            watchdog_running.store(true);
            auto last = player.Alive(); auto ui_moved = started, worker_moved = started;
            while (!watchdog_stop.load()) {
                Sleep(250);
                const auto now = GetTickCount64(); const auto alive = player.Alive();
                if (alive.ui_beats != last.ui_beats || alive.ui != last.ui) ui_moved = now;
                if (alive.worker_beats != last.worker_beats || alive.worker != last.worker) worker_moved = now;
                last = alive;
                using Phase = media::engine::PlaybackCoordinator::Phase;
                const bool worker_waits = alive.worker == Phase::WorkerStopped || alive.worker == Phase::WorkerTeardown || alive.worker == Phase::WorkerStart;
                const char* why = now - ui_moved > 20000 ? "the presentation thread made no progress for 20 s" :
                                  now - worker_moved > 20000 && !worker_waits ? "the media worker made no progress for 20 s" :
                                  limit && now - started > (limit + 15) * 1000ull ? "the run outlived its limit by 15 s" : nullptr;
                if (!why) continue;
                std::printf("WATCHDOG: %s; presentation thread: %s (%llu passes), media worker: %s (%llu passes)\n", why,
                            media::engine::PlaybackCoordinator::Name(alive.ui), static_cast<unsigned long long>(alive.ui_beats),
                            media::engine::PlaybackCoordinator::Name(alive.worker), static_cast<unsigned long long>(alive.worker_beats));
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        });
    }
    // --test-attach (API 17, subtitles after open in the window): a file opened without subtitles; at 1 s an .srt is attached from
    // another thread, at 3 s a .vtt replaces it; a file without a cue is refused and changes nothing.
    struct AttachRun { int first = -1, second = -1, empty = -1; unsigned presents_before = 0, presents_first = 0; bool first_shown = false, second_shown = false; } attach;
    std::thread attacher; std::atomic<bool> attacher_running{false};
    if (mode == Mode::Attach) {
        const auto write = [](const char* path, const char* text) { std::FILE* f = nullptr; if (fopen_s(&f, path, "wb") || !f) return; std::fputs(text, f); std::fclose(f); };
        write("attach_one.srt", "1\r\n00:00:00,500 --> 00:00:59,000\r\nAttached while playing.\r\n\r\n");
        write("attach_two.vtt", "WEBVTT\n\n00:00:00.500 --> 00:00:59.000\nA second file replaces it.\n\n");
        write("attach_empty.srt", "no cue in this file\n");
        attacher = std::thread([&] {
            attacher_running.store(true);
            const auto limit = GetTickCount64() + 15000;
            const auto wait = [&](std::int64_t at) { while (player.PublishedState().position < at && GetTickCount64() < limit) Sleep(10); };
            wait(10000000);
            attach.presents_before = player.SubtitlePresents();
            attach.first = static_cast<int>(player.AttachSubtitles("attach_one.srt"));
            wait(20000000);
            attach.presents_first = player.SubtitlePresents();
            const auto has = [&](std::string_view text) { for (const auto& c : player.Captions().Cues()) if (c.text == text) return true; return false; };
            attach.first_shown = has("Attached while playing.");
            attach.empty = static_cast<int>(player.AttachSubtitles("attach_empty.srt"));
            wait(30000000);
            attach.second = static_cast<int>(player.AttachSubtitles("attach_two.vtt"));
            attach.second_shown = has("A second file replaces it.") && !has("Attached while playing.");
        });
    }
    while (mode == Mode::Attach && !attacher_running.load()) Sleep(1); // Its start-up allocates: before measurement.
    while (test && !watchdog_running.load()) Sleep(1); // Its start-up allocates (per-thread CRT data): before measurement.
    // Every way out of here stops it (also the early returns below).
    struct WatchdogGuard { std::atomic<bool>& stop; std::thread& thread; ~WatchdogGuard() { stop.store(true); if (thread.joinable()) thread.join(); } } watchdog_guard{watchdog_stop, watchdog};
    const auto begin = GetTickCount64();
    allocation_probe::Begin();
    const bool played = player.Run();
    if (attacher.joinable()) attacher.join();
    const auto allocations = allocation_probe::End();
    fault.stop.store(true); if (injector.joinable()) injector.join();
    const auto wall_ms = GetTickCount64() - begin;
    std::printf("Playback: frames=%u dropped=%u hidden=%u discarded=%u out-of-order=%u PCM frames=%u allocations=%llu stage=%u native=%08lx validated=%u\n",
        player.Presented(), player.Dropped(), player.Hidden(), player.Discarded(), player.OutOfOrder(), player.AudioFrames(),
        static_cast<unsigned long long>(allocations), player.Error(), static_cast<unsigned long>(player.NativeError()), player.ValidatedFrames());
    if (player.OutOfOrder()) {
        static constexpr const char* reasons[]{"inserted", "full", "duplicate", "late"};
        const auto& o = player.Renderer().LastOrderReject();
        std::printf("Last out-of-order: pts=%.4fs epoch=%u (renderer epoch %u) %s%s; last released %.4fs\n", o.pts / 1e7, o.epoch, o.renderer_epoch,
                    reasons[o.reason & 3], o.pinned ? " (pinned turn)" : "", o.last_released == INT64_MIN ? -1.0 : o.last_released / 1e7);
    }
    if (player.Dropped()) {
        const auto& d = player.Renderer().LastDrop();
        std::printf("Last drop: pts=%.4fs late by %.1fms (%s; waited for%s)\n", d.pts / 1e7, d.lateness / 1e4,
                    d.behind_clock ? "already behind the clock" : "missed its deadline", Waited(d.waited));
    }
    std::printf("Clock: re-anchors=%u underrun silence frames=%llu wall=%llums; tone-mapped frames=%u\n", player.Discontinuities(),
                static_cast<unsigned long long>(player.SilenceFrames()), static_cast<unsigned long long>(wall_ms), player.ToneMapped());
    std::printf("Audio lead: underruns bridged=%u, read past waiting video %u times, least PCM queued %.1f ms\n", player.BridgedGaps(), player.ReadAheads(),
                static_cast<double>(player.LeastAudioLead()) / 1e4);
    {
        // Modules (docs/sdk.md): each enabled module's region of the arena, and what it used of it.
        static constexpr const char* names[]{"voice", "HUD", "subtitles", "neural", "chapters", "telemetry"};
        const auto arena = player.ArenaStats();
        std::printf("Arena: %.1f of %.1f MiB in use; modules", arena.used_bytes / 1048576.0, arena.capacity_bytes / 1048576.0);
        for (unsigned m = 0; m < media::engine::kModuleCount; ++m) {
            const auto region = player.ModuleArenaStats(m);
            if (region.capacity_bytes) std::printf(" %s %.3f/%.2f MiB", names[m], region.high_water_bytes / 1048576.0, region.capacity_bytes / 1048576.0);
        }
        std::printf("; %u threads\n", player.Threads());
    }
    // Pre-roll: the clock starts after the first picture has been handed to the swap chain.
    {
        const auto& worst = player.Renderer().WorstFlip();
        std::printf("Pre-roll: first picture %.1fms, clock started %.1fms after Run with %u pictures decoded; latest flip %.2fms after its deadline (pts=%.4fs; waited for%s); "
                    "warm-up flip %.1fms (Present %.1fms; DwmFlush %08lx)\n",
                    player.FirstFrameTime() / 1e4, player.ClockStart() / 1e4, player.PrerollPictures(), worst.lateness / 1e4, worst.pts / 1e7, Waited(worst.waited),
                    player.Renderer().WarmUp() / 1e4, player.Renderer().WarmUpPresent() / 1e4, static_cast<unsigned long>(player.Renderer().WarmUpResult()));
    }
    if (player.Dropped() || GetEnvironmentVariableW(L"MEDIA_STARTUP_TRACE", nullptr, 0) != 0) {
        // The start-up trace (docs/fixtures.md, "Cold starts"): times from the first picture reaching the renderer, ms.
        const media::render::D3D12Renderer::TraceConversion* c = nullptr; const media::render::D3D12Renderer::TracePicture* t = nullptr;
        const unsigned conversions = player.Renderer().TraceConversions(c), pictures = player.Renderer().TracePictures(t);
        const std::int64_t origin = conversions ? c[0].seen : 0;
        const auto ms = [origin](std::int64_t at) { return at ? (at - origin) / 1e4 : -1.0; };
        std::puts("Start-up trace: conversions (pts s: seen, decoded, queued, read ms; GPU ms; tier; slots busy)");
        for (unsigned i = 0; i < conversions; ++i)
            std::printf("  c%-2u %.4f: %8.2f %8.2f %8.2f %8.2f  gpu %6.2f  %c %u\n", i, c[i].pts / 1e7, ms(c[i].seen), ms(c[i].decoded), ms(c[i].queued), ms(c[i].harvested),
                        c[i].gpu / 1e4, "NEF"[c[i].tier % 3], c[i].busy);
        std::puts("Start-up trace: pictures (pts s: deadline, flipped or dropped ms, late ms, waited for)");
        for (unsigned i = 0; i < pictures; ++i)
            std::printf("  p%-2u %.4f: %8.2f %8.2f %s late %6.2f%s\n", i, t[i].pts / 1e7, ms(t[i].deadline), ms(t[i].at), t[i].dropped ? "DROPPED" : "flipped",
                        t[i].deadline ? (t[i].at - t[i].deadline) / 1e4 : 0.0, Waited(t[i].waited));
    }
    {
        const auto& r = player.Renderer(); const auto& g = r.Governor();
        std::printf("Quality: mode %s, tier %s; GPU per conversion p95 %.2fms (last %.2fms, worst %.2fms) of a %.1fms interval, timestamps %s; "
                    "changes %u (down %u, of them %u on a thin queue; up %u, manual %u)\n", media::render::Name(r.Quality()), media::render::Name(r.Tier()), g.P95() / 1e4,
                    r.LastGpuTime() / 1e4, g.Stats().worst / 1e4, g.Budget() / 1e4, r.Timing() ? "on" : "off", g.Changes(), g.Stats().steps_down,
                    g.Stats().thin, g.Stats().steps_up, g.Stats().manual);
    }
    {
        const auto& c = player.LongestCalls();
        std::printf("Longest calls on the presentation thread: convert %.2fms (at %.4fs), present %.2fms (at %.4fs), window messages %.2fms (at %.4fs)\n",
                    c.convert / 1e4, c.convert_pts / 1e7, c.present / 1e4, c.present_pts / 1e7, c.messages / 1e4, c.messages_pts / 1e7);
        const auto mean = [](std::int64_t sum, std::uint32_t n) { return n ? static_cast<double>(sum) / n / 1e4 : 0.0; };
        std::printf("Mean calls on the presentation thread: convert %.3fms (%u), present %.3fms (%u), window messages %.3fms (%u passes)\n",
                    mean(c.convert_sum, c.converts), c.converts, mean(c.present_sum, c.presents), c.presents, mean(c.messages_sum, c.passes), c.passes);
        // Low-power pacing: each thread's CPU time as a share of one core, while playing and while paused.
        const auto& p = player.PacingStats();
        const auto share = [](std::int64_t cpu, std::int64_t wall) { return wall > 0 ? 100.0 * static_cast<double>(cpu) / static_cast<double>(wall) : 0.0; };
        std::printf("Pacing: %s; playing %.1fs: presentation %.1f%%, worker %.1f%%, process %.1f%%; paused %.1fs: presentation %.1f%%, worker %.1f%%, process %.1f%%; "
                    "%u waits (%.1fs, %u ended early; paused %u, settled %u), timer late %.3fms mean, %.3fms worst (%u over 1 ms, %u over 4 ms, %u past the deadline by %.3fms mean, %.3fms most; spin %uus)\n", options.low_power_pacing ? "low-power" : "busy spin",
                    p.playing.wall / 1e7, share(p.playing.presentation, p.playing.wall), share(p.playing.worker, p.playing.wall), share(p.playing.process, p.playing.wall),
                    p.paused.wall / 1e7, share(p.paused.presentation, p.paused.wall), share(p.paused.worker, p.paused.wall), share(p.paused.process, p.paused.wall),
                    p.waits, p.waited / 1e7, p.early, p.paused_waits, p.settled_waits, p.timed ? static_cast<double>(p.late_sum) / p.timed / 1e4 : 0.0, p.late_max / 1e4, p.late_1ms, p.late_4ms, p.past_deadline,
                    p.past_deadline ? static_cast<double>(p.past_sum) / p.past_deadline / 1e4 : 0.0, p.past_max / 1e4, options.pace_spin_us);
        std::printf("Pacing spins: off %llu, moving %llu, convert %llu, present %llu, deadline %llu, controls %llu; worker: %u waits (%.1fs); "
                    "controls-only images while paused: %u\n",
                    static_cast<unsigned long long>(p.spins[0]), static_cast<unsigned long long>(p.spins[1]), static_cast<unsigned long long>(p.spins[2]),
                    static_cast<unsigned long long>(p.spins[3]), static_cast<unsigned long long>(p.spins[4]), static_cast<unsigned long long>(p.spins[5]),
                    p.worker_waits, p.worker_waited / 1e7, player.PausedHudPresents());
    }
    {
        // The window thread (docs/session.md, "The window thread"): input from its queueing to its handling.
        const auto& in = player.Renderer().InputStatistics();
        std::printf("Input: %s; %u events handled, waited %.3f ms mean, %.3f ms at most; %u lost\n",
                    player.Renderer().MessageThread() ? "window thread" : "presentation thread (no window thread)", in.handled,
                    in.handled ? static_cast<double>(in.latency_sum) / in.handled / 1e4 : 0.0, in.latency_max / 1e4, in.lost);
    }
    if (player.Remote()) {
        const auto n = player.Network();
        std::printf("Network: %llu bytes in %u range GETs (%u read-ahead, %u retries, %u failures), %u cached hits; file %llu bytes%s\n",
                    static_cast<unsigned long long>(n.bytes), n.gets, n.prefetches, n.retries, n.failures, n.hits,
                    static_cast<unsigned long long>(served.size()), slow ? ", 15 ms latency, 50 Mbit/s" : "");
    }
    {
        const auto& r = player.Routing();
        const char* status = r.status == media::engine::AudioStatus::Ready ? "ready" : r.status == media::engine::AudioStatus::UnsupportedLayout ?
                             "unsupported layout (video plays without audio)" : "no audio track";
        if (r.status != media::engine::AudioStatus::None)
            std::printf("Audio: %s; source %u ch mask %03x -> decoded %u ch mask %03x; endpoint %u ch mask %03x -> output %u ch%s%s\n", status,
                        r.source_channels, r.source_mask, r.decoded_channels, r.decoded_mask, r.endpoint_channels, r.endpoint_mask, r.output_channels,
                        r.fold_down ? " (DSP fold-down)" : "", r.converting ? " (endpoint converts)" : "");
    }
    if (player.SubtitleState() != media::render::SubtitleStatus::Off) {
        const auto& st = player.Renderer().Subtitles().Stats();
        std::printf("Subtitles: %s (native %08lx); cells=%u resident=%u distinct=%u generated=%u evictions=%u dropped=%u composites=%u cues shown=%u of %zu\n",
                    StatusName(player.SubtitleState()), static_cast<unsigned long>(player.SubtitleError()), st.cells, st.glyphs, st.unique, st.generated,
                    st.evictions, st.dropped, player.SubtitlePresents(), player.Renderer().CuesShown(), player.Captions().Cues().size());
        std::printf("Startup: subtitle pipeline prepared in %.2fms; first frame %.1fms after Run\n", player.SubtitlePrepareTime() / 1e4, player.FirstFrameTime() / 1e4);
    }
    if (player.Search().Ready())
        std::printf("Subtitle search: %zu cues indexed in %zu bytes, built in %.3fms\n", player.Captions().Cues().size(), player.Search().Bytes(), player.SearchPrepareTime() / 1e4);
    for (unsigned i = 0; i < player.CommandResults(); ++i) {
        const auto& r = player.CommandAt(i);
        std::printf("Command %u: %s %s in %.1fus; volume %.0f%%%s%s", i + 1, media::ai::Name(r.intent.kind), media::ai::Name(r.intent.status), r.resolve / 10.0,
                    r.volume * 100, r.muted ? " muted" : "", r.paused ? ", paused" : "");
        if (r.epoch) std::printf("; landed=%.4fs shown as %.4fs with %u cue(s), %.2fms", r.pts / 1e7, r.subtitles.time / 1e7, r.subtitles.count, r.latency / 1e4);
        std::puts("");
    }
    // Generated fixtures: 72 video pictures at 24 fps and 141 AAC-LC access units (144384 PCM frames).
    // Every decoded picture is validated; cut pictures are decoded as references but never presented.
    const unsigned hidden = mode == Mode::Trim ? 1u : 0u;
    // Surround fixtures: 72 pictures; the 5.1 track decodes about 141 access units, the 7.1 track none.
    const bool surround_fixture = mode == Mode::Surround || mode == Mode::Surround71;
    // Seeking modes replay or skip parts of the timeline, so whole-file frame counts do not apply.
    // A device loss re-decodes from a sync sample and skips the frames that fell due while the device was rebuilt.
    const bool seeking = mode == Mode::Seek || mode == Mode::SeekForward || mode == Mode::SeekMicro || mode == Mode::Hud || mode == Mode::Scrub || mode == Mode::Dsp || mode == Mode::Subtitles ||
                         mode == Mode::SubtitleScrub || mode == Mode::LongForm || mode == Mode::RemoteSeek || mode == Mode::DeviceLoss || mode == Mode::DeviceLossPaused ||
                         mode == Mode::Intents || mode == Mode::Palette || mode == Mode::Voice || mode == Mode::Tracks || mode == Mode::TracksSurround ||
                         mode == Mode::Quality || mode == Mode::Chapters || mode == Mode::FollowUps || mode == Mode::NeuralReuse || mode == Mode::PausedQuality ||
                         mode == Mode::Speed || mode == Mode::Zoom || mode == Mode::ZoomLive || mode == Mode::AbLoop || mode == Mode::Telemetry ||
                         mode == Mode::Bookmarks || mode == Mode::Rotation || mode == Mode::WindowStall || mode == Mode::AutoHorizon || mode == Mode::CropBars;
    const bool fixture = mode == Mode::Fixture || mode == Mode::FixtureSearch || mode == Mode::FixtureHdr || mode == Mode::Pacing || mode == Mode::Attach;
    bool complete = !test || seeking || fixture || audio_loss || (player.Presented() == 72 - hidden && player.Hidden() == hidden && player.ValidatedFrames() == 72 &&
                              (surround_fixture || player.AudioFrames() == 144384) && player.Dropped() == 0 && player.OutOfOrder() == 0 &&
                              // HEVC fixtures carry B-pictures: the SPS reorder depth must reach the presentation window.
                              (!player.Hevc() || player.ReorderDepth() >= 1));
    if (seeking) complete = player.Dropped() == 0 && player.OutOfOrder() == 0 && player.Presented() > 0;
    if (mode == Mode::Seek || mode == Mode::Dsp || mode == Mode::Subtitles) {
        // Each landing is the frame whose display interval contains the target, reached from its sync sample.
        complete = complete && player.Seeks() == 2 && player.SeekResults() == 2;
        for (unsigned i = 0; i < player.SeekResults(); ++i) {
            const auto& r = player.SeekAt(i);
            // Audio restarts on the target sample itself (pre-roll trimmed), within one 48 kHz sample.
            const bool audio = std::llabs(r.audio_start - r.target) <= 10000000 / 48000 + 1;
            const bool exact = r.landed && r.pts <= r.target && r.target < r.pts + r.duration && audio;
            std::printf("Seek %u: target=%.4fs landed=%.4fs audio=%.6fs %s latency=%.2fms clock-resumed=%.2fms %s\n", i + 1, r.target / 1e7,
                        r.pts / 1e7, r.audio_start / 1e7, r.paused ? "(paused)" : "(playing)", r.latency / 1e4,
                        r.resumed ? (r.resumed - r.requested) / 1e4 : -1.0, exact ? "exact" : "WRONG");
            complete = complete && exact && r.resumed != 0;
            PrintWork(player, r.epoch);
            if (mode == Mode::Subtitles) complete = CaptionsMatch(player, r.subtitles, Landing(r.target, r.pts, r.duration)) && complete;
        }
        complete = complete && player.SeekAt(1).paused && player.Presented() >= 50;
    }
    if (mode == Mode::SeekMicro) {
        // Queue seeks: 1.0 -> 1.4 s and 1.6 -> 2.1 s (their landings already converted) decode nothing
        // again; after a seek back to 0.9 s, the first one again with queue seeks off restarts from the sync sample.
        complete = complete && player.SeekResults() == 4;
        const media::engine::PlaybackCoordinator::SeekWork* work[4]{};
        for (unsigned i = 0; i < player.SeekResults() && i < 4; ++i) {
            const auto& r = player.SeekAt(i);
            const bool audio = std::llabs(r.audio_start - r.target) <= 10000000 / 48000 + 1;
            const bool exact = r.landed && r.pts <= r.target && r.target < r.pts + r.duration && audio;
            work[i] = player.WorkFor(r.epoch);
            std::printf("Seek %u: target=%.4fs landed=%.4fs audio=%.6fs %s latency=%.2fms clock-resumed=%.2fms %s%s\n", i + 1, r.target / 1e7,
                        r.pts / 1e7, r.audio_start / 1e7, r.paused ? "(paused)" : "(playing)", r.latency / 1e4,
                        r.resumed ? (r.resumed - r.requested) / 1e4 : -1.0, exact ? "exact" : "WRONG",
                        work[i] && work[i]->adopted ? (work[i]->kept ? "; queue seek, landing already converted" : "; queue seek, landing decoded") : "");
            PrintWork(player, r.epoch);
            complete = complete && exact && r.resumed != 0 && work[i];
        }
        if (complete) {
            const auto& a = player.SeekAt(0); const auto& d = player.SeekAt(3);
            const bool queue = work[0]->adopted && work[0]->references == 0 && work[0]->skipped == 0 && work[1]->adopted && work[1]->references == 0 &&
                               !work[2]->adopted && !work[3]->adopted && player.AdoptedSeeks() == 2 && player.Renderer().AdoptedFrames() > 0;
            std::printf("Queue seek: landing %.2fms instead of %.2fms restarting (%u reference pictures decoded then); %u pictures adopted\n",
                        a.latency / 1e4, d.latency / 1e4, work[3]->references, player.Renderer().AdoptedFrames());
            complete = queue && a.latency < d.latency;
        }
        complete = complete && player.Dropped() == 0 && player.OutOfOrder() == 0;
    }
    if (mode == Mode::SeekForward) {
        // The same forward seek (1.5 s -> 2.5 s, playing) twice: carried on from the decode position, then restarted
        // from the sync sample. Both land exactly; the continuation decodes fewer reference pictures and lands sooner.
        complete = complete && player.SeekResults() == 3;
        const media::engine::PlaybackCoordinator::SeekWork* work[3]{};
        for (unsigned i = 0; i < player.SeekResults() && i < 3; ++i) {
            const auto& r = player.SeekAt(i);
            const bool audio = std::llabs(r.audio_start - r.target) <= 10000000 / 48000 + 1;
            const bool exact = r.landed && r.pts <= r.target && r.target < r.pts + r.duration && audio;
            std::printf("Seek %u: target=%.4fs landed=%.4fs audio=%.6fs %s latency=%.2fms clock-resumed=%.2fms %s\n", i + 1, r.target / 1e7,
                        r.pts / 1e7, r.audio_start / 1e7, r.paused ? "(paused)" : "(playing)", r.latency / 1e4,
                        r.resumed ? (r.resumed - r.requested) / 1e4 : -1.0, exact ? "exact" : "WRONG");
            PrintWork(player, r.epoch);
            work[i] = player.WorkFor(r.epoch);
            complete = complete && exact && r.resumed != 0 && work[i];
        }
        if (complete) {
            const auto& a = player.SeekAt(0); const auto& b = player.SeekAt(2);
            const bool fewer = work[0]->continued && work[0]->resumed > 0 && !work[1]->continued && !work[2]->continued &&
                               work[0]->references < work[2]->references;
            std::printf("Forward continuation: %u fewer reference pictures decoded (%u instead of %u), %u resumed; landing %.2fms instead of %.2fms (%.0f%%)\n",
                        work[2]->references - work[0]->references, work[0]->references, work[2]->references, work[0]->resumed,
                        a.latency / 1e4, b.latency / 1e4, b.latency ? 100.0 * static_cast<double>(a.latency) / static_cast<double>(b.latency) : 0.0);
            complete = fewer && a.latency < b.latency && player.ContinuedSeeks() == 1;
        }
        complete = complete && player.Dropped() == 0 && player.OutOfOrder() == 0;
    }
    if (mode == Mode::RemoteSeek) {
        // A 10-minute B-picture Matroska file over HTTP with its Cues at the end: five landings, each exact with its sound, each a
        // cue seek on an index that covers only what was read; then the end of the stream. Checked below with the network.
        const auto duration = player.Bounds().duration_100ns;
        std::printf("Remote: timeline %.3fs, %u GETs, %llu of %zu bytes read (%.1f %%), %u unauthorized, %u failures\n", duration / 1e7,
                    server.gets.load(), static_cast<unsigned long long>(player.Network().bytes), served.size(),
                    100.0 * static_cast<double>(player.Network().bytes) / static_cast<double>(served.size() ? served.size() : 1),
                    server.unauthorized.load(), player.Network().failures);
        complete = complete && http && player.Container() == media::demux::ContainerFormat::Matroska && player.ReorderDepth() >= 1 &&
                   duration >= 5999000000 && duration <= 6001000000 && player.Seeks() == 5 && player.SeekResults() == 5 &&
                   server.unauthorized.load() == 0 && player.Network().bytes * 2 <= served.size();
        for (unsigned i = 0; i < player.SeekResults(); ++i) {
            const auto& r = player.SeekAt(i);
            const bool audio = std::llabs(r.audio_start - r.target) <= 10000000 / 48000 + 1;
            const bool exact = r.landed && r.pts <= r.target && r.target < r.pts + r.duration && audio;
            std::printf("Seek %u: target=%.4fs landed=%.4fs audio=%.6fs %s latency=%.2fms clock-resumed=%.2fms %s\n", i + 1, r.target / 1e7,
                        r.pts / 1e7, r.audio_start / 1e7, r.paused ? "(paused)" : "(playing)", r.latency / 1e4,
                        r.resumed ? (r.resumed - r.requested) / 1e4 : -1.0, exact ? "exact" : "WRONG");
            complete = complete && exact && r.resumed != 0 && r.paused == (i == 2);
        }
    }
    if (mode == Mode::LongForm) {
        // 1 h, 2 h, 45 min (paused), 3 h 30 min: each landing exact, as in --test-seek, then played to the end.
        const std::uint64_t samples = player.IndexedSamples();
        std::printf("Index: %llu samples in %llu bytes (%.2f B/sample)\n", static_cast<unsigned long long>(samples),
                    static_cast<unsigned long long>(player.IndexBytes()), samples ? static_cast<double>(player.IndexBytes()) / static_cast<double>(samples) : 0.0);
        complete = complete && samples > 800000 && player.Seeks() == 4 && player.SeekResults() == 4;
        for (unsigned i = 0; i < player.SeekResults(); ++i) {
            const auto& r = player.SeekAt(i);
            const bool audio = std::llabs(r.audio_start - r.target) <= 10000000 / 48000 + 1;
            const bool exact = r.landed && r.pts <= r.target && r.target < r.pts + r.duration && audio;
            std::printf("Seek %u: target=%.4fs landed=%.4fs audio=%.6fs %s latency=%.2fms clock-resumed=%.2fms %s\n", i + 1, r.target / 1e7,
                        r.pts / 1e7, r.audio_start / 1e7, r.paused ? "(paused)" : "(playing)", r.latency / 1e4,
                        r.resumed ? (r.resumed - r.requested) / 1e4 : -1.0, exact ? "exact" : "WRONG");
            complete = complete && exact && r.resumed != 0;
        }
        complete = complete && player.SeekAt(2).paused && !player.SeekAt(3).paused;
    }
    if (mode == Mode::Scrub || mode == Mode::SubtitleScrub) {
        const auto& s = player.Scrub();
        const bool final_exact = s.pts <= s.target && s.target < s.pts + s.duration;
        const bool burst_exact = s.burst_pts <= s.burst_target && s.burst_target < s.burst_pts + s.burst_duration &&
                                 std::llabs(s.burst_audio - s.burst_target) <= 10000000 / 48000 + 1;
        std::printf("Scrub: requests=%u sent=%u applied=%u coalesced=%u superseded=%u frames-while-dragging=%u (%u keyframe previews) max-gap=%.1fms\n",
                    s.requests, player.Seeks(), player.AppliedSeeks(), player.CoalescedScrubs(), player.SupersededSeeks(), s.landings, s.previews, s.max_gap / 1e4);
        std::printf("Scrub halves: forward frames=%u max-gap=%.1fms; backward frames=%u max-gap=%.1fms; preview seeks applied=%u (preview %s)\n",
                    s.forward_landings, s.forward_gap / 1e4, s.backward_landings, s.backward_gap / 1e4, player.PreviewSeeks(), scrub_preview ? "on" : "off");
        std::printf("Scrub release: target=%.4fs landed=%.4fs final-latency=%.2fms %s (%s); burst of 4: superseded=%u landed=%.4fs audio=%.6fs %s\n",
                    s.target / 1e7, s.pts / 1e7, s.final_latency / 1e4, final_exact ? "exact" : "WRONG", s.released ? "release seek" : "last scrub seek was exact",
                    s.burst_superseded, s.burst_pts / 1e7, s.burst_audio / 1e7, burst_exact ? "exact" : "WRONG");
        // MEDIA_SCRUB_TRACE=1: every frame shown while dragging.
        if (GetEnvironmentVariableW(L"MEDIA_SCRUB_TRACE", nullptr, 0) != 0)
            for (unsigned i = 0; i < s.landings && i < s.kMaxTrace; ++i)
                std::printf("  drag frame %2u at %6.1fms: latency=%6.2fms pts=%.4fs target=%.4fs%s\n", i, s.trace[i].at / 1e4, s.trace[i].latency / 1e4,
                            s.trace[i].pts / 1e7, s.trace[i].target / 1e7, s.trace[i].preview ? " preview" : "");
        // Every request (the release included) is either sent or coalesced; each sent seek is applied or superseded; the
        // drag showed frames; with previews off, none was a preview; the release left no preview on screen.
        complete = complete && s.done && final_exact && burst_exact && s.burst_superseded == 3 && s.landings >= 3 &&
                   player.CoalescedScrubs() > 0 && s.requests + (s.released ? 1u : 0u) + 4 == player.Seeks() + player.CoalescedScrubs() &&
                   player.Seeks() == player.AppliedSeeks() + player.SupersededSeeks() && (scrub_preview || player.PreviewSeeks() == 0) &&
                   (!expect_previews || (s.previews >= 1 && s.backward_landings >= 1)) && !player.ShowingPreview();
    }
    // Resilience: a genuine endpoint underrun (inserted silence) must be absorbed by re-anchoring the clock.
    if (mode == Mode::Dsp) {
        const auto& dsp = player.DspStats();
        std::printf("DSP: frames=%llu peak=%g boost=%gdB gain=%gdB resets=%u clarity=%u spatial=%u\n",
                    static_cast<unsigned long long>(dsp.frames), static_cast<double>(dsp.peak), static_cast<double>(dsp.max_boost_db),
                    static_cast<double>(dsp.min_gain_db), player.DspResets(), player.ClarityLevel(), player.SpatialAudio() ? 1u : 0u);
        complete = complete && dsp.frames > 48000 && dsp.peak > 0 && dsp.peak <= 0.99999994f &&
                   player.DspResets() == 2 && player.ClarityLevel() == 1 && player.SpatialAudio();
    }
    if (mode == Mode::Subtitles || mode == Mode::SubtitleScrub) {
        std::printf("GPU subtitles: composites=%u\n",player.SubtitlePresents());
        complete = complete && player.SubtitlePresents() > 30;
    }
    // Subtitle faults degrade the captions, never the playback (the whole-file checks above still apply).
    if (mode == Mode::SubtitleOverflow) {
        const auto& st = player.Renderer().Subtitles().Stats();
        // Twice the atlas capacity, paged through it while playing: every cue shown whole (no glyph left out, none
        // replaced), glyphs evicted and generated again, and the pipeline's preparation off the startup path.
        complete = complete && player.SubtitleState() == media::render::SubtitleStatus::Ready && st.unique > 2 * st.cells &&
                   st.cells == media::render::SdfSubtitlePipeline::kAtlasCells && st.dropped == 0 && st.evictions > 0 &&
                   player.Renderer().CuesShown() == player.Captions().Cues().size() && player.SubtitlePrepareTime() < 200000;
    }
    if (mode == Mode::SubtitleMissing) complete = complete && player.SubtitleState() == media::render::SubtitleStatus::Disabled && player.SubtitlePresents() == 0;
    if (mode == Mode::Surround) {
        // 5.1 decoded as 6 channels (FL FR FC LFE BL BR), sent as is or folded to stereo, S toggled three times live.
        const auto& r = player.Routing(); const auto& dsp = player.DspStats();
        const auto expected = options.surround == media::engine::SurroundOutput::Passthrough ? 6u :
                              options.surround == media::engine::SurroundOutput::Stereo ? 2u : r.fold_down ? 2u : 6u;
        std::printf("Surround: %u PCM frames; DSP frames=%llu peak=%g spatial=%u toggles=%u\n", player.AudioFrames(),
                    static_cast<unsigned long long>(dsp.frames), static_cast<double>(dsp.peak), player.SpatialAudio() ? 1u : 0u, player.SpatialToggles());
        complete = complete && r.status == media::engine::AudioStatus::Ready && r.source_channels == 6 && r.source_mask == 0x3f &&
                   r.decoded_channels == 6 && r.decoded_mask == 0x3f && r.output_channels == expected && r.fold_down == (expected == 2) &&
                   player.AudioFrames() >= 130 * 1024 && player.SpatialToggles() == 3 && dsp.peak > 0 && dsp.peak < 1 &&
                   (!r.fold_down || dsp.frames >= player.AudioFrames()); // Fold-down processes every frame.
    }
    if (mode == Mode::Neural) {
        // Fullscreen (so the picture is upscaled), N presses through every mode (Neural, Edge-directed, Fast, Auto, and
        // Neural again) and two C toggles (guided chroma off, on), fullscreen off: every reconstruction ran, with every
        // frame on time and nothing allocated.
        const auto& r = player.Renderer();
        const bool fixed = r.Quality() == media::render::QualityMode::Neural;
        std::printf("Neural: available=%u frames converted through the neural pass=%u of %llu, now %s; slots %ux%u\n", r.NeuralAvailable() ? 1u : 0u,
                    r.NeuralFrames(), static_cast<unsigned long long>(r.Converted()), fixed ? "on" : "off", r.SlotWidth(), r.SlotHeight());
        std::printf("Guided chroma: %s\n", r.GuidedChroma() ? "on" : "off");
        // V on at the first N, off at the third step: the split comparison was converted, and is off at the end. Temporal
        // tile reuse: consecutive neural pictures of an epoch may reuse unchanged tiles of the one before.
        std::printf("Comparison: %u pictures converted split, %s at the end; neural pictures allowed to reuse tiles: %u of %u\n", r.ComparisonFrames(),
                    r.Comparison() ? "on" : "off", r.NeuralReuseFrames(), r.NeuralFrames());
        complete = complete && r.NeuralAvailable() && fixed && r.NeuralFrames() > 5 && r.NeuralFrames() + 5 < r.Converted() && r.GuidedChroma() &&
                   r.ComparisonFrames() > 5 && !r.Comparison() && r.NeuralReuseFrames() > 0 && r.NeuralReuseFrames() < r.NeuralFrames();
    }
    if (mode == Mode::Quality) {
        // The governor's script: Auto steps up to Neural on headroom; under the extra load it steps down twice, to Fast,
        // and stays there (no oscillation); N fixes Neural, then three N presses return to Auto. Every change it made
        // is in order and in its phase; every picture on time; a cue for each change.
        using media::render::QualityTier; using Reason = media::render::QualityGovernor::Reason;
        const auto& r = player.Renderer(); const auto& g = r.Governor(); const auto& q = player.QualityRun();
        bool sequence = g.Changes() == 6;
        const struct { QualityTier from, to; Reason reason; std::int64_t after, before; } expected[]{
            {QualityTier::EdgeDirected, QualityTier::Neural, Reason::Headroom, 0, q.load_on},
            {QualityTier::Neural, QualityTier::EdgeDirected, Reason::Overload, q.load_on, q.load_off},
            {QualityTier::EdgeDirected, QualityTier::Fast, Reason::Overload, q.load_on, q.load_off},
            {QualityTier::Fast, QualityTier::Neural, Reason::Manual, q.load_off, q.auto_again},
            {QualityTier::Neural, QualityTier::EdgeDirected, Reason::Manual, q.load_off, INT64_MAX},
            {QualityTier::EdgeDirected, QualityTier::Fast, Reason::Manual, q.load_off, INT64_MAX}};
        for (unsigned i = 0; i < g.Changes() && i < media::render::QualityGovernor::kHistory; ++i) {
            const auto& c = g.ChangeAt(i);
            const char* why = c.reason == Reason::Overload ? "overload" : c.reason == Reason::Headroom ? "headroom" : c.reason == Reason::Manual ? "manual" : "unavailable";
            std::printf("Quality change %u: %s -> %s (%s) at %.2fs, p95 %.2fms of %.1fms\n", i + 1, media::render::Name(c.from), media::render::Name(c.to), why,
                        (c.at - q.load_on) / 1e7 + 4.0, c.p95 / 1e4, c.budget / 1e4);
            if (i < std::size(expected)) {
                const auto& e = expected[i];
                sequence = sequence && c.from == e.from && c.to == e.to && c.reason == e.reason && c.at >= e.after && c.at < e.before;
            }
        }
        std::printf("Quality frames: neural=%u edge-directed=%u fast=%u; status cues shown=%u\n", r.TierFrames(QualityTier::Neural),
                    r.TierFrames(QualityTier::EdgeDirected), r.TierFrames(QualityTier::Fast), r.StatusShown());
        complete = complete && r.Timing() && sequence && r.Quality() == media::render::QualityMode::Auto && r.Tier() == QualityTier::Fast &&
                   r.TierFrames(QualityTier::Neural) > 10 && r.TierFrames(QualityTier::EdgeDirected) > 10 && r.TierFrames(QualityTier::Fast) > 10 &&
                   r.StatusShown() >= 5 && player.Dropped() == 0 && player.Presented() > 250;
    }
    if (mode == Mode::Surround71) {
        // 7.1 AAC (a PCE-described 8-channel stream) is recognised and dropped: the programme still plays, silently.
        const auto& r = player.Routing();
        complete = complete && r.status == media::engine::AudioStatus::UnsupportedLayout && r.source_channels == 8 && r.source_mask == 0x63f &&
                   player.AudioFrames() == 0;
    }
    if (mode == Mode::Resize) {
        // Each step: the swap chain has the new client size, the square video is letterboxed and centred, and the dock
        // keeps its scale rule, side margin and bottom margin. Video kept playing through every step.
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        (void)GetMonitorInfoW(MonitorFromWindow(player.Renderer().Window(), MONITOR_DEFAULTTONEAREST), &monitor);
        const unsigned mw = static_cast<unsigned>(monitor.rcMonitor.right - monitor.rcMonitor.left), mh = static_cast<unsigned>(monitor.rcMonitor.bottom - monitor.rcMonitor.top);
        const unsigned expected[5][2]{{960, 540}, {540, 960}, {1600, 686}, {mw, mh}, {1600, 686}};
        const float dpi = player.Renderer().DpiScale();
        complete = complete && player.ResizeSteps() == 5;
        for (unsigned i = 0; i < player.ResizeSteps(); ++i) {
            const auto& r = player.ResizeAt(i); const unsigned w = expected[i][0], h = expected[i][1], side = w < h ? w : h;
            const float scale = media::ui::KineticHud::LayoutScale(w, h, dpi);
            const bool size = r.applied && r.width == w && r.height == h && r.fullscreen == (i == 3);
            const bool video = r.video.width == side && r.video.height == side && r.video.x == (w - side) / 2 && r.video.y == (h - side) / 2;
            const bool dock = r.hud_scale == scale && r.dock_left >= 16 * scale - 0.01f && std::fabs(static_cast<float>(h) - r.dock_bottom - 26 * scale) < 0.01f;
            const bool playing = r.presented_after > r.presented_before;
            std::printf("Resize %u: %ux%u%s video %ux%u at (%u,%u) dock scale %.3f left %.1f bottom margin %.1f frames %u->%u %s\n", i + 1, r.width, r.height,
                        r.fullscreen ? " fullscreen" : "", r.video.width, r.video.height, r.video.x, r.video.y, static_cast<double>(r.hud_scale),
                        static_cast<double>(r.dock_left), static_cast<double>(static_cast<float>(r.height) - r.dock_bottom), r.presented_before, r.presented_after,
                        size && video && dock && playing ? "ok" : "WRONG");
            complete = complete && size && video && dock && playing;
        }
        std::printf("Swap-chain resizes=%u scaled composites=%u slot capacity %ux%u\n", player.Renderer().Resizes(), player.Renderer().ScaledComposites(),
                    player.Renderer().SlotWidth(), player.Renderer().SlotHeight());
        complete = complete && player.Renderer().Resizes() >= 5 && !player.Renderer().Fullscreen();
    }
    if (mode == Mode::SubtitleScrub) {
        const auto& s = player.Scrub();
        complete = CaptionsMatch(player, s.subtitles, Landing(s.target, s.pts, s.duration)) &&
                   CaptionsMatch(player, s.burst_subtitles, Landing(s.burst_target, s.burst_pts, s.burst_duration)) && complete;
    }
    if (mode == Mode::DeviceLoss || mode == Mode::DeviceLossPaused) {
        // One real device removal, noticed by one of the two threads, rebuilt with every reference to the lost device
        // gone, and decoding resumed in the same epoch at the frame on screen (paused) or at the clock (playing).
        const auto& r = player.LastDeviceRecovery(); const auto& landing = player.RecoveryLanding();
        std::printf("Rebuild stages: worker parked after %.1fms, lost device released in %.1fms, renderer %.1fms, decoder %.1fms\n",
                    (r.parked - r.lost) / 1e4, (r.released - r.parked) / 1e4, (r.recreated - r.released) / 1e4, (r.rebuilt - r.recreated) / 1e4);
        const bool paused = mode == Mode::DeviceLossPaused;
        std::printf("Device loss: injected=%08lx reason=%08lx noticed by %s; rebuilt in %.1fms (%u attempt%s), %lu references left on the lost device\n",
                    static_cast<unsigned long>(fault.result), static_cast<unsigned long>(r.reason), r.detector == 1 ? "presentation" : r.detector == 2 ? "decode" : "nobody",
                    (r.rebuilt - r.lost) / 1e4, r.attempts, r.attempts == 1 ? "" : "s", static_cast<unsigned long>(r.leaked));
        // Playing: the first frame on the new device is due at or after the clock at the rebuild (the display floor
        // follows the clock while decoding catches up); none of the following frames is late (see the drop count).
        const bool exact = landing.presented && landing.epoch == 1 && (paused ? landing.pts == r.shown && r.target == r.shown :
                           r.target != INT64_MIN && landing.pts + landing.duration > r.target);
        std::printf("Recovery landing: %s shown=%.4fs target=%.4fs landed=%.4fs epoch %u, %.1fms after the rebuild %s\n", r.paused ? "(paused)" : "(playing)",
                    r.shown / 1e7, r.target / 1e7, landing.pts / 1e7, landing.epoch, landing.latency / 1e4, exact ? "exact" : "WRONG");
        complete = complete && SUCCEEDED(fault.result) && player.DeviceRecoveries() == 1 && player.Renderer().Recoveries() == 1 &&
                   r.reason == DXGI_ERROR_DEVICE_REMOVED && r.detector != 0 && r.attempts == 1 && r.leaked == 0 && r.paused == paused && exact &&
                   player.Renderer().Device() && SUCCEEDED(player.Renderer().Device()->GetDeviceRemovedReason()); // The replacement is healthy.
        if (paused) {
            // The held picture came back while still paused, and the pause itself re-anchored the clock once.
            complete = complete && player.Presented() >= 72 && player.Dropped() == 0 && player.OutOfOrder() == 0 && player.Discontinuities() >= 1;
        } else {
            // Audio played straight through the rebuild: no silence, and the clock never had to re-anchor. Captions
            // came from the rebuilt atlas. How much of the clip is left after the rebuild depends on the driver: freeing
            // a removed device with 4K decode in flight took 2.1 s once on the UHD 630 (the landing is then the last frame).
            const unsigned after = player.SubtitlePresents() - r.subtitle_presents;
            std::printf("Continuity: silence frames=%llu re-anchors=%u; subtitle composites after the rebuild=%u\n",
                        static_cast<unsigned long long>(player.SilenceFrames()), player.Discontinuities(), after);
            complete = complete && player.SilenceFrames() == 0 && player.Discontinuities() == 0 && player.OutOfOrder() == 0 && after >= 1;
            unsigned expected[media::subtitle::SubtitleTrack::kMaxActive]{};
            const unsigned count = player.Captions().Select(landing.pts, expected);
            bool same = landing.subtitles.time == landing.pts && landing.subtitles.count == count;
            for (unsigned i = 0; same && i < count; ++i) same = landing.subtitles.cues[i] == expected[i];
            std::printf("Captions at the recovery landing %.4fs: %u cue(s), %s\n", landing.pts / 1e7, count, same ? "match" : "WRONG");
            complete = complete && same;
        }
        complete = complete && player.AudioFrames() == 144384;
    }
    if (audio_loss) {
        // The shared stream was invalidated (in-process, or for real by an exclusive-mode stream), playback continued on
        // the null endpoint while endpoints refused to open, and moved back to one afterwards, with the clock stepping by
        // at most 50 ms.
        const auto& a = player.AudioRecovery();
        std::printf("Audio loss: %s %s after %u attempt(s) (%08lx); losses=%u (%u injected, %08lx) detachments=%u migrations=%u last open=%08lx attached=%u\n",
                    mode == Mode::AudioSteal ? "exclusive hold" : "in-process invalidation", fault.held ? "taken" : "NOT taken", fault.attempts,
                    static_cast<unsigned long>(fault.result), a.losses, a.injected, static_cast<unsigned long>(a.last_loss), a.detachments, a.migrations,
                    static_cast<unsigned long>(a.last_open), player.AudioAttached() ? 1u : 0u);
        const auto& c = player.Continuity();
        std::printf("Clock continuity over %u endpoint handover(s): skipped %.2fms (lost endpoint buffer), repeated %.2fms, held %.2fms (endpoint start)\n",
                    c.handovers, c.skip / 1e4, c.repeat / 1e4, c.hold / 1e4);
        // Every picture presented, or dropped to keep up with the skipped PCM of the lost endpoint (at most one each).
        const bool pictures = player.Presented() + player.Dropped() == 72 && player.Dropped() <= a.losses && player.ValidatedFrames() == 72 &&
                              player.OutOfOrder() == 0 && player.AudioFrames() == 144384;
        complete = pictures && fault.held && a.losses >= 1 && a.last_loss == AUDCLNT_E_DEVICE_INVALIDATED && a.detachments >= 1 &&
                   a.migrations >= 1 && player.AudioAttached() && c.handovers >= 1 && c.skip <= 500000 && c.repeat <= 0 &&
                   a.injected == (mode == Mode::AudioLoss ? 1u : 0u) && a.last_open == S_OK;
    }
    if (fault_mode) {
        std::printf("Audio settings after recovery: clarity=%u spatial=%u\n", player.ClarityLevel(), player.SpatialAudio() ? 1u : 0u);
        complete = complete && player.ClarityLevel() == 2 && player.SpatialAudio();
    }
    {
        const auto& t = player.Timeline();
        if (t.Chapters() > 1 || t.Scenes() || mode == Mode::Chapters) {
            const auto& y = player.Synthesis();
            std::printf("Timeline: %s, %u chapter(s), %u scene(s) (%u live; structure %u, dialogue %u; cadence %.3fs), prepared in %.2fms; %u dock mark(s)\n",
                        media::timeline::Name(t.Source()), t.Chapters(), t.Scenes(), t.LiveScenes(), y.structural, y.dialogue, y.cadence / 1e7,
                        player.TimelinePrepareTime() / 1e4, player.Renderer().Marks().count);
            for (unsigned c = 0; c < t.Chapters() && c < 8; ++c) std::printf("  chapter %u at %.3fs \"%s\"\n", c + 1, t.ChapterAt(c).start / 1e7, t.ChapterAt(c).title);
            for (unsigned c = 0; c < t.Scenes() && c < 8; ++c) std::printf("  scene %u at %.3fs (%s)\n", c + 1, t.SceneAt(c).at / 1e7, media::timeline::Name(t.SceneAt(c).origin));
            for (unsigned i = 0; i < player.Navigations(); ++i) {
                const auto& n = player.NavigationAt(i);
                std::printf("  navigation %u: %s %s from %.3fs -> %s %.3fs \"%s\"\n", i + 1, n.direction > 0 ? "next" : "previous", n.chapter ? "chapter" : "scene",
                            n.from / 1e7, media::ai::Name(n.status), n.target / 1e7, n.cue);
            }
        }
    }
    if (mode == Mode::Chapters) {
        // Each command and the key went where its cue says, landing exactly; both cuts were seen while playing; the dock
        // has a tick at each chapter after the start; the chapter track (QuickTime) was not offered as subtitles.
        const auto& t = player.Timeline();
        bool navigated = player.Navigations() == std::size(kChapterCues);
        for (unsigned i = 0; navigated && i < std::size(kChapterCues); ++i) {
            const auto& n = player.NavigationAt(i);
            navigated = std::strcmp(n.cue, kChapterCues[i]) == 0 && (kChapterTargets[i] < 0 ? n.status == media::ai::IntentStatus::NotFound :
                                                                    n.status == media::ai::IntentStatus::Ok && n.target == kChapterTargets[i]);
            if (!navigated) std::printf("FAIL navigation %u: \"%s\" (expected \"%s\")\n", i + 1, n.cue, kChapterCues[i]);
        }
        bool landed = player.CommandResults() == std::size(kChapterScript);
        for (unsigned i = 0; landed && i < player.CommandResults(); ++i) {
            const auto& r = player.CommandAt(i);
            landed = r.intent.status == media::ai::IntentStatus::NotFound ? !r.epoch : r.landed;
        }
        // Landings: every navigation that seeks is the frame showing its target.
        for (unsigned i = 0; landed && i < player.CommandResults(); ++i) {
            const auto& r = player.CommandAt(i);
            if (!r.epoch) continue;
            const auto& n = player.NavigationAt(i < 6 ? i : i + 1);
            landed = r.pts <= n.target && n.target < r.pts + r.duration;
        }
        unsigned near_cuts = 0;
        for (unsigned c = 0; c < t.Scenes(); ++c)
            if (t.SceneAt(c).origin == media::timeline::SceneOrigin::Live) {
                const auto at = t.SceneAt(c).at;
                if (std::llabs(at - 30000000) <= 420000 || std::llabs(at - 60000000) <= 420000) ++near_cuts;
            }
        const auto& marks = player.Renderer().Marks();
        const bool ticks = marks.count >= 2 && marks.Chapter(0) && marks.Chapter(1) && std::fabs(marks.Fraction(0) - 1.0 / 3) < 0.01 &&
                           std::fabs(marks.Fraction(1) - 2.0 / 3) < 0.01;
        bool chapter_track = true;
        for (unsigned i = 0; i < player.Tracks(); ++i)
            if (player.TrackAt(i).kind == media::demux::TrackKind::Subtitle) chapter_track = chapter_track && player.TrackAt(i).use == media::engine::TrackUse::Chapters;
        std::printf("Chapters: navigations %s, landings %s, live cuts near 3 s and 6 s: %u (renderer cuts %u, luma pictures %u), ticks %s, chapter track %s\n",
                    navigated ? "as expected" : "WRONG", landed ? "exact" : "WRONG", near_cuts, player.Renderer().Cuts(), player.Renderer().LumaPictures(),
                    ticks ? "placed" : "WRONG", chapter_track ? "not a subtitle track" : "OFFERED AS SUBTITLES");
        complete = complete && navigated && landed && near_cuts == 2 && t.LiveScenes() == 2 && t.Chapters() == 3 && ticks && chapter_track &&
                   (t.Source() == media::timeline::ChapterSource::Nero || t.Source() == media::timeline::ChapterSource::QuickTime) &&
                   std::strcmp(t.ChapterAt(1).title, "\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa9\xd7\xa0\xd7\x99") == 0;
    }
    if (mode == Mode::Isolation) {
        // The desktop's input left no trace: no seek, no pause, no fullscreen, no quality change, no command bar, and the
        // window was not activated. The harness's tagged posts did arrive (Down x4: 100 % -> 80 %).
        const auto& r = player.Renderer();
        std::printf("Isolation: %s; desktop messages posted=%u dropped=%u, WM_MOUSEACTIVATE answered %ld; seeks=%u paused=%u fullscreen=%u quality=%s "
                    "bar=%u volume=%.0f%% foreground=%u\n", r.InputIsolation() ? "on" : "off", fault.attempts, r.IsolatedInputs(), static_cast<long>(fault.activation),
                    player.Seeks(), player.Paused() ? 1u : 0u, r.Fullscreen() ? 1u : 0u, media::render::Name(r.Quality()), r.Palette().IsOpen() ? 1u : 0u,
                    player.Volume() * 100, GetForegroundWindow() == r.Window() ? 1u : 0u);
        complete = complete && SUCCEEDED(fault.result) && fault.attempts >= 30 && r.IsolatedInputs() >= fault.attempts &&
                   fault.activation == MA_NOACTIVATEANDEAT && player.Seeks() == 0 && !player.Paused() && player.Discontinuities() == 0 &&
                   !r.Fullscreen() && r.Quality() == media::render::QualityMode::Auto && !r.Palette().IsOpen() && r.PaletteSubmits() == 0 &&
                   std::fabs(player.Volume() - 0.80f) < 1e-6f && !player.Muted() && GetForegroundWindow() != r.Window();
    }
    if (mode == Mode::Volume) {
        // Key presses reached the engine (90 %, unmuted at the end); every change ramped over 20 ms (steps of at most
        // 1/960 at 48 kHz), and the gain never raised a sample.
        const auto& g = player.GainStats();
        std::printf("Volume: %.0f%% (%.1f dB) %s; %u ramps, largest per-frame gain step %.6f, peak %.4f, %llu frames scaled\n", player.Volume() * 100,
                    static_cast<double>(player.VolumeDecibels()), player.Muted() ? "muted" : "unmuted", g.ramps, static_cast<double>(g.max_step),
                    static_cast<double>(g.peak), static_cast<unsigned long long>(g.frames));
        complete = complete && player.Volume() == 0.9f && !player.Muted() && g.ramps >= 4 && g.max_step <= 1.0f / 960 + 1e-7f && g.peak < 1 && g.frames > 48000;
    }
    if (mode == Mode::PausedQuality) {
        // While paused, each key changes the picture on screen at once, and exactly as its setting says: V (a refresh
        // landing, nothing being held yet) splits it, the right half the same picture as before, the left the plain
        // conversion; N (re-converted from the held source) changes the right half only; C likewise (guided chroma off);
        // V again restores the left half to the enhanced conversion and keeps the right half.
        const auto& r = player.Renderer();
        const char* keys[]{"V (comparison on)", "N (Edge-directed)", "C (guided chroma off)", "V (comparison off)"};
        bool good = r.Captures() == 5 && r.PausedUpdates() == 4;
        for (unsigned i = 0; i < r.PausedUpdates() && i < 4; ++i) {
            const auto& u = r.PausedUpdateAt(i);
            std::printf("Paused update %u, %-22s %s: presented %.1f ms, GPU done %.1f ms after the key; tier %s, chroma %s, comparison %s\n", i + 1, keys[i],
                        u.refresh ? "refresh landing" : "re-converted", u.latency / 1e4, u.completed / 1e4, media::render::Name(u.tier),
                        u.chroma_mode ? "guided" : "bilinear", u.compare ? "on" : "off");
        }
        for (unsigned i = 0; i < r.Captures(); ++i) std::printf("Capture %u: %ux%u left %016llx right %016llx\n", i, r.CaptureAt(i).width, r.CaptureAt(i).height,
                                                                 static_cast<unsigned long long>(r.CaptureAt(i).left), static_cast<unsigned long long>(r.CaptureAt(i).right));
        if (good) {
            const auto same = [&](unsigned a, unsigned b, bool left) { return left ? r.CaptureAt(a).left == r.CaptureAt(b).left : r.CaptureAt(a).right == r.CaptureAt(b).right; };
            using media::render::QualityTier;
            const auto& u = [&](unsigned i) -> const auto& { return r.PausedUpdateAt(i); };
            const bool modes = u(0).refresh && u(0).compare && u(0).tier == QualityTier::Neural && u(0).chroma_mode && !u(1).refresh && u(1).compare &&
                               u(1).tier == QualityTier::EdgeDirected && !u(2).refresh && !u(2).chroma_mode && u(2).compare && !u(3).refresh && !u(3).compare;
            const bool pixels = !same(1, 0, true) && same(1, 0, false) && same(2, 1, true) && !same(2, 1, false) && same(3, 2, true) &&
                                !same(4, 3, true) && same(4, 3, false);
            std::printf("Paused inspection: modes %s; halves %s (C changed the right half: %s); refresh landings %u\n", modes ? "as set" : "WRONG",
                        pixels ? "as expected" : "WRONG", same(3, 2, false) ? "no" : "yes", player.PausedRefreshes());
            good = modes && pixels && player.PausedRefreshes() == 1;
            for (unsigned i = 1; i < 4; ++i) good = good && u(i).completed > 0 && u(i).completed < 500000; // On screen, GPU done, within 50 ms of the key.
        }
        complete = complete && good;
    }
    if (mode == Mode::Music) {
        // --test-music (docs/audio-dsp.md, "Music mode"): the file plays as sound alone, its sound drawn at the display's
        // rate; each chapter's tone is the loudest band shown in it (440 Hz, 1760 Hz after "next chapter", 880 Hz at 1.5x);
        // the chapter lands on its start; every picture handed over was being heard (its block began at most 45 ms of media
        // before the clock, never after it), none came while paused, none was dropped; nothing allocated, to the end.
        const auto& m = player.MusicRun();
        const double refresh = 1e7 / static_cast<double>(player.Renderer().RefreshPeriod());
        const double mean = m.shown ? static_cast<double>(m.lag_sum) / m.shown / 1e4 : -1.0;
        std::printf("Music: mode %s, %u pictures made (%u dropped), %u shown (%u passed over, %u of a left epoch), lag %.1f..%.1f ms (mean %.1f), "
                    "%.1f composites a second (display %.1f Hz), %u composites in all\n", player.MusicMode() ? "on" : "OFF", m.published, m.dropped, m.shown, m.skipped,
                    m.stale, m.shown ? m.lag_min / 1e4 : -1.0, m.lag_max / 1e4, mean, m.rate, refresh, player.Renderer().MusicPresents());
        std::printf("Music: loudest bands %u %u %u (the tones' %u %u %u); chapter landed at %.4fs in %.1f ms; while paused %u pictures, %u composites\n",
                    m.bands[0], m.bands[1], m.bands[2], m.expected[0], m.expected[1], m.expected[2], m.landing / 1e7, m.landing_latency / 1e4,
                    m.paused_pictures, m.paused_presents);
        const bool bands = m.bands[0] == m.expected[0] && m.bands[1] == m.expected[1] && m.bands[2] == m.expected[2];
        const bool landed = m.landing >= 30000000 && m.landing <= 30500000 && m.landing_latency <= 3000000;
        const bool timed = m.shown > 100 && m.lag_min >= 0 && m.lag_max <= 450000 && mean <= 25.0;
        complete = m.done && player.MusicMode() && bands && landed && timed && m.dropped == 0 && m.paused_pictures == 0 && m.rate >= 0.8 * refresh &&
                   player.Presented() == 0 && player.Dropped() == 0;
    }
    if (mode == Mode::WindowStall) {
        // --test-window-stall (docs/session.md, "The window thread"): three 120 ms stalls of the window's procedure while
        // playing; no picture dropped, and input (a test mouse move every 250 ms) handled within 4 ms of being queued.
        const auto& in = player.Renderer().InputStatistics();
        std::printf("Window stalls: %u of 120 ms posted; %u pictures presented, %u dropped; input %u events, at most %.3f ms behind\n", player.WindowStalls(),
                    player.Presented(), player.Dropped(), in.handled, in.latency_max / 1e4);
        complete = player.WindowStalls() == 3 && player.Dropped() == 0 && player.Presented() >= 200 && (!player.Renderer().MessageThread() || (in.handled >= 10 && in.latency_max <= 40000));
    }
    if (mode == Mode::AutoHorizon || mode == Mode::CropBars) {
        // --test-auto-horizon (docs/picture.md, "Auto-leveling the horizon"): the fixture's 640x352 picture tilted +4.5 degrees
        // (clockwise) for 3 s, then -6.0. Before Shift+R, the picture as stored; at 2.8 s the leveling turn -4.5 and at 5.8 s
        // +6.0 (within 0.35 degree), found from the GPU's histograms alone (one 1024-byte readback a picture), and the shown
        // picture's own lines level (within 0.35 degree, measured on the screen capture: a test hook). Each capture's 4 x 4 grid
        // against the stored picture sampled through video::Orientation::Map (in the view's region), as the rotation test does.
        // --test-crop-bars (docs/picture.md, "Black bars"): the same with 40 rows of black encoded at the top and the bottom
        // (--horizon-letterbox): leveled the same, the bars' edges ignored; before the crop the shown image has the bars (and its
        // lines, bars and all, read +4.5); after "recadrer les bandes noires" the bars are found (rows 40 to 312, within 2), the
        // view is cropped inside them and the shown image has picture from edge to edge.
        const auto& r = player.AutoHorizonRun();
        const bool boxed = mode == Mode::CropBars;
        constexpr unsigned kBarRows = 40;
        static constexpr double kTruth[]{4.5, 4.5, -6.0};
        std::vector<std::uint8_t> stored[2];
        horizon_scene::Draw(4.5, 640, 352, stored[0]); horizon_scene::Draw(-6.0, 640, 352, stored[1]);
        if (boxed) for (auto& p : stored) horizon_scene::Bars(p, 640, 352, kBarRows, 0);
        bool good = r.done && r.count == 3;
        for (unsigned i = 0; i < r.count && i < 3; ++i) {
            const auto& s = r.steps[i];
            const auto& picture = stored[kTruth[i] > 0 ? 0 : 1];
            const auto at = [&](double x, double y) {
                const int cx = static_cast<int>((std::clamp)(x, 0.0, 639.999)), cy = static_cast<int>((std::clamp)(y, 0.0, 351.999));
                return (picture[static_cast<std::size_t>(cy) * 640 + static_cast<unsigned>(cx)] - 16) * 255.0 / 219.0;
            };
            const double rw = s.region.right - s.region.left, rh = s.region.bottom - s.region.top;
            double m[4], t[2]; s.orientation.Map(s.width, s.height, rw > 0 ? rw : 640, rh > 0 ? rh : 352, m, t);
            t[0] += s.region.left; t[1] += s.region.top; // The region's samples from the crop's origin.
            double worst = 0;
            for (unsigned cell = 0; cell < 16; ++cell) {
                const double x0 = (cell % 4) * s.width / 4.0, y0 = (cell / 4) * s.height / 4.0, cw = s.width / 4.0, ch = s.height / 4.0;
                double sum = 0;
                for (unsigned j = 0; j < 8; ++j)
                    for (unsigned k = 0; k < 8; ++k) {
                        const double X = x0 + (k + 0.5) * cw / 8, Y = y0 + (j + 0.5) * ch / 8;
                        sum += at(m[0] * X + m[1] * Y + t[0], m[2] * X + m[3] * Y + t[1]);
                    }
                worst = (std::max)(worst, std::fabs(sum / 64 - s.grid[cell]));
            }
            std::printf("Auto horizon at %.2f s (truth %+.1f degrees): auto %s, level %+.1f, the horizon's tilt %+.2f (%s); shown %ux%u, its own lines %s %+.2f "
                        "(mass %.0f); grid worst cell %.1f codes from the prediction\n", s.position / 1e7, kTruth[i], s.on ? "on" : "off", static_cast<double>(s.level),
                        static_cast<double>(s.tilt), s.locked ? "locked" : "not locked", s.width, s.height, s.shown.found ? "at" : "not measured,",
                        static_cast<double>(s.shown.tilt), static_cast<double>(s.shown.mass), worst);
            good = good && worst <= 24;
            if (i == 0) good = good && !s.on && s.level == 0 && s.shown.found && std::fabs(s.shown.tilt - kTruth[0]) <= 0.5; // As stored.
            // Leveled with the bars still shown, the bars' edges turn with the picture (-4.5 degrees on screen) and the shown image's
            // lines are theirs as much as the scene's: there the grid's match checks the geometry.
            else good = good && s.on && s.locked && std::fabs(s.level + kTruth[i]) <= 0.35 && std::fabs(s.tilt - kTruth[i]) <= 0.35 &&
                        ((boxed && i == 1) || (s.shown.found && std::fabs(s.shown.tilt) <= 0.35));
            if (boxed) {
                const auto& e = s.extents; const auto& a = s.active;
                std::printf("  bars %s, active (%u, %u)-(%u, %u); view region (%u, %u)-(%u, %u)%s; the shown image has picture in (%u, %u)-(%u, %u) of %ux%u\n",
                            s.bars ? "found" : "not found", a.left, a.top, a.right, a.bottom, s.region.left, s.region.top, s.region.right, s.region.bottom,
                            s.cropped ? ", cropped" : "", e.left, e.top, e.right, e.bottom, s.width, s.height);
                const auto close = [](unsigned v, unsigned want) { return (v > want ? v - want : want - v) <= 2; };
                const bool edge_to_edge = s.extents_found && e.left <= 1 && e.top <= 1 && e.right + 1 >= s.width && e.bottom + 1 >= s.height;
                // The bars shown as stored at first (leveled, they turn with the picture and a corner of the view reaches past them:
                // the shown image's bounding box no longer tells), then cropped away.
                if (i == 0) good = good && !s.cropped && s.extents_found && e.top >= s.height * kBarRows / 352 - 3;
                else if (i == 1) good = good && !s.cropped;
                else good = good && s.cropped && s.bars && close(a.left, 0) && close(a.top, kBarRows) && close(a.right, 640) && close(a.bottom, 352 - kBarRows) &&
                            s.region.top >= a.top && s.region.bottom <= a.bottom && edge_to_edge;
            }
        }
        std::printf("Auto horizon: %u pictures measured from %u histogram readbacks (1024 bytes each, no picture read back for it)\n", r.measured, r.readbacks);
        complete = good && r.measured >= 60 && player.Dropped() == 0;
    }
    if (mode == Mode::Rotation) {
        // --test-rotation (docs/picture.md, "Rotation"): every picture as the file's orientation and the viewer's turns predict
        // it. The rotation fixture's 64x32 picture: dark left half (60), light right half (180), a white square (235) at
        // (2..13, 2..13). Each 4 x 4 cell of a captured picture against the same cell of the prediction: the stored picture
        // sampled through video::Orientation::Map (the conversion's own map) at 8 x 8 points of the cell, limited range to full.
        const auto& r = player.RotationRun();
        const auto stored = [](double x, double y) {
            const double cx = (std::clamp)(x, 0.0, 63.999), cy = (std::clamp)(y, 0.0, 31.999);
            const double code = cx >= 2 && cx < 14 && cy >= 2 && cy < 14 ? 235 : cx < 32 ? 60 : 180;
            return (code - 16) * 255.0 / 219.0;
        };
        bool good = r.done && r.count == 5;
        static constexpr unsigned kTurns[]{0, 1, 2, 2, 3};   // The viewer's turns at each picture.
        static constexpr float kLevels[]{0, 0, 0, 3, 0};
        for (unsigned i = 0; i < r.count && i < 5; ++i) {
            const auto& s = r.steps[i];
            double m[4], t[2]; s.orientation.Map(s.width, s.height, 64, 32, m, t);
            double worst = 0;
            for (unsigned cell = 0; cell < 16; ++cell) {
                const double x0 = (cell % 4) * s.width / 4.0, y0 = (cell / 4) * s.height / 4.0, cw = s.width / 4.0, ch = s.height / 4.0;
                double sum = 0;
                for (unsigned j = 0; j < 8; ++j)
                    for (unsigned k = 0; k < 8; ++k) {
                        const double X = x0 + (k + 0.5) * cw / 8, Y = y0 + (j + 0.5) * ch / 8;
                        sum += stored(m[0] * X + m[1] * Y + t[0], m[2] * X + m[3] * Y + t[1]);
                    }
                worst = (std::max)(worst, std::fabs(sum / 64 - s.grid[cell]));
            }
            const bool upright_shape = s.width > s.height, across = s.orientation.Across();
            std::printf("Rotation picture %u (%s): %ux%u, shown %u quarter turn(s)%s, level %.1f degrees; grid", i, s.paused ? "paused" : "playing", s.width, s.height,
                        s.orientation.QuarterTurns(), s.orientation.Mirrored() ? " mirrored" : "", static_cast<double>(s.orientation.level));
            for (unsigned cell = 0; cell < 16; ++cell) std::printf("%s%.0f", cell % 4 ? " " : cell ? " | " : " ", static_cast<double>(s.grid[cell]));
            std::printf("; worst cell %.1f codes from the prediction\n", worst);
            good = good && worst <= 24 && s.orientation.turns == kTurns[i] && s.orientation.level == kLevels[i] && upright_shape != across &&
                   s.paused == (i >= 2 && i <= 4);
        }
        complete = good && player.Dropped() == 0;
    }
    if (mode == Mode::Zoom) {
        // --test-zoom (docs/picture.md, "Zoom"): the zoom each step asked for, its region where the step puts it (the wheel
        // keeps the sample under the pointer under it; the pan moves a tenth of the region), each picture changed by its
        // step, the reset picture the very one before any zoom, every paused re-conversion within 50 ms, nothing dropped.
        const auto& z = player.ZoomRun();
        const auto& c = player.Picture();
        bool good = z.done && z.count == 5;
        const float zooms[]{1.0f, 2.0f, 2.0f * 1.953125f, 2.0f * 1.953125f, 1.0f};
        for (unsigned i = 0; i < z.count && i < 5; ++i) {
            const auto& st = z.steps[i];
            std::printf("Zoom step %u: %.3fx at (%.3f, %.3f), region (%u,%u)-(%u,%u), image %016llx %016llx, re-converted in %.1f ms\n", i, static_cast<double>(st.zoom),
                        static_cast<double>(st.x), static_cast<double>(st.y), st.region.left, st.region.top, st.region.right, st.region.bottom,
                        static_cast<unsigned long long>(st.left), static_cast<unsigned long long>(st.right), st.latency / 1e4);
            good = good && std::fabs(st.zoom - zooms[i]) < 1e-3f && (i == 0 || st.latency <= 500000);
            if (i && i < 4) good = good && (st.left != z.steps[i - 1].left || st.right != z.steps[i - 1].right);
        }
        if (z.count == 5) {
            const auto& r0 = z.steps[0].region; const auto& r1 = z.steps[1].region; const auto& r2 = z.steps[2].region; const auto& r3 = z.steps[3].region; const auto& r4 = z.steps[4].region;
            const unsigned w = c.right - c.left, h = c.bottom - c.top;
            const bool whole = r0.left == c.left && r0.top == c.top && r0.right == c.right && r0.bottom == c.bottom && r4.left == r0.left && r4.right == r0.right && r4.bottom == r0.bottom;
            const bool half = r1.right - r1.left == w / 2 && r1.bottom - r1.top == h / 2 && r1.left == c.left + w / 4 && r1.top == c.top + h / 4;
            // The wheel at 3/4 across: the sample shown there before (in region 1) is shown there after (in region 2).
            const double before = r1.left + 0.75 * (r1.right - r1.left), after = r2.left + 0.75 * (r2.right - r2.left);
            const double pan = static_cast<double>(r2.left) - r3.left, tenth = 0.1 * (r2.right - r2.left);
            const bool wheel = std::fabs(before - after) <= 2.0 && std::fabs(r2.top - (r1.top + ((r1.bottom - r1.top) - (r2.bottom - r2.top)) / 2.0)) <= 2.0;
            const bool panned = std::fabs(pan - tenth) <= 2.0 && r3.top == r2.top && r3.right - r3.left == r2.right - r2.left;
            const bool restored = z.steps[4].left == z.steps[0].left && z.steps[4].right == z.steps[0].right;
            std::printf("Zoom geometry: whole %s, half %s, wheel point %.1f -> %.1f %s, pan %.1f of %.1f %s, reset image %s\n", whole ? "ok" : "WRONG", half ? "ok" : "WRONG",
                        before, after, wheel ? "ok" : "WRONG", pan, tenth, panned ? "ok" : "WRONG", restored ? "identical" : "DIFFERENT");
            good = good && whole && half && wheel && panned && restored;
        }
        complete = good && player.Dropped() == 0;
    }
    if (mode == Mode::Tracking) {
        // --test-tracking (docs/picture.md, "Target tracking"), on the tracking fixture: T at 1 s locks onto the square in the
        // middle; the view goes to 2x on the very next picture shown (the queue converted before the lock is shown through the
        // tracking view, live zoom) and follows the square right as it moves from 1 s to 4 s, the audio focus with it; at the cut
        // to the dark shot (5 s) the tracker is lost and the view holds where it was, with the "Tracking lost" cue.
        const auto& run = player.TrackRecord();
        unsigned followed = 0, close = 0, held = 0, after_cut = 0, audio_with_view = 0; float worst = 0, before_cut = -1; double worst_at = 0, worst_truth = 0, worst_view = 0; std::int64_t first_moved = -1; double tracker_worst = 0; unsigned tracker_samples = 0; unsigned zoomed_at = UINT32_MAX;
        for (unsigned i = 0; i < run.count; ++i) {
            const auto& s = run.samples[i];
            const auto frame = static_cast<long long>((s.pts * 24 + 5000000) / 10000000), even = frame & ~1ll;
            const double t = static_cast<double>(even) / 24.0;
            const double left = 26 + std::lround(std::clamp((t - 1.0) / 3.0, 0.0, 1.0) * 16.0);
            const double truth = std::clamp((left + 6) / 64.0, 0.25, 0.75); // At 2x the view stays inside the picture.
            // From the first picture shown zoomed by T (samples[0] is the picture on screen when T was pressed).
            if (first_moved < 0 && s.shown_zoom > 1.5f) { first_moved = s.pts; zoomed_at = i; }
            if (first_moved >= 0 && s.pts < 50000000) {
                ++followed; const float e = static_cast<float>(std::fabs(s.shown_x - truth)); if (e < 3.5f / 64) ++close; // 3.5 samples: the tracker's 1-sample cells, the region's even-sample alignment.
                if (e > worst) { worst = e; worst_at = s.pts / 1e7; worst_truth = truth; worst_view = s.shown_x; }
                if (std::fabs(s.audio_x - s.shown_x) < 0.02f) ++audio_with_view;
                before_cut = s.shown_x;
            }
            if (s.tracked_pts >= 14000000 && s.tracked_pts < 50000000 && s.state == 1) { // The tracker's own estimate, for its picture.
                const auto f2 = static_cast<long long>((s.tracked_pts * 24 + 5000000) / 10000000) & ~1ll;
                const double l2 = 26 + std::lround(std::clamp((static_cast<double>(f2) / 24.0 - 1.0) / 3.0, 0.0, 1.0) * 16.0);
                tracker_worst = (std::max)(tracker_worst, std::fabs(s.tracked_x - (l2 + 6) / 64.0)); ++tracker_samples;
            }
            if (s.pts >= 52000000) { ++after_cut; if (s.state == 2 && std::fabs(s.shown_x - before_cut) < 0.002f) ++held; }
        }
        const auto& t_zoom = player.Renderer().LiveZoomAt(0); // T's zoom to 2x, a live zoom.
        const double refresh = static_cast<double>(player.Renderer().RefreshPeriod());
        std::printf("Tracking: T answered on picture %u after the one on screen (%.3f s), %.2f ms after the key (%.2f refreshes), %u queued pictures shown through the tracking view; %u pictures followed up to 5 s, %u within 3.5 samples of the square (worst %.3f at %.3f s: view %.3f, square %.3f), the audio focus on the view %u; "
                    "after the cut %u pictures, %u held at %.3f while lost; cue \"%s\"; tracker events %u\n",
                    zoomed_at, first_moved / 1e7, t_zoom.shown / 1e4, t_zoom.shown / refresh, t_zoom.bridged, followed, close, worst, worst_at, worst_view, worst_truth, audio_with_view, after_cut, held, before_cut, run.last_cue, run.events);
        std::printf("  the tracker's estimate for the picture it measured: worst %.3f (%.1f samples) over %u pictures\n", tracker_worst, tracker_worst * 64, tracker_samples);
        complete = tracker_samples > 60 && tracker_worst < 3.0 / 64 && zoomed_at == 1 && player.Renderer().LiveZooms() >= 1 && t_zoom.shown > 0 &&
                   t_zoom.shown <= static_cast<std::int64_t>(2 * refresh) && t_zoom.bridged > 0 && followed > 60 && close == followed && audio_with_view == followed && after_cut > 10 && held == after_cut &&
                   std::strcmp(run.last_cue, "Tracking lost \xc2\xb7 Holding view") == 0 && player.Dropped() == 0;
    }
    if (mode == Mode::Vibrometry || mode == Mode::VibrometryChroma) {
        // --test-vibrometry (docs/picture.md, "Remote vibrometry"): the pulse fixture's patch at 72 beats a minute, read while
        // tracking (T; the region follows the target) and with micro-motion on (the view's fovea); the HUD said so. Since
        // 2026-11-03 from the GPU's full-resolution samples; --test-vibrometry-chroma on the chroma-pulse fixture (luma still,
        // Cr pulsing): the pulse found in Cr both ways (a luma grid saw nothing there).
        const auto& v = player.VibrometryRecord();
        bool good = v.tracked && v.micro_on && v.tracking.taken && v.micro.taken;
        static constexpr const char* kChannels[]{"luma", "Cb", "Cr", "shift x", "shift y"};
        for (const auto* r : {&v.tracking, &v.micro}) {
            const bool right = std::fabs(r->bpm - 72.0f) <= 1.5f && r->confidence > 0.5f && r->readouts > 0 && std::strstr(r->text, "72 BPM") != nullptr &&
                               (mode != Mode::VibrometryChroma || r->channel == media::video::VibrometryResult::kCr);
            std::printf("Vibrometry %s: %.2f BPM (confidence %.2f, %s) over %.2f s at %.1f a second; %u readouts, the last \"%s\" %s\n",
                        r == &v.tracking ? "while tracking" : "with micro-motion", r->bpm, r->confidence, r->channel < 5 ? kChannels[r->channel] : "?", r->seconds, r->rate,
                        r->readouts, r->text, right ? "right" : "WRONG");
            good = good && right;
        }
        complete = good && player.Dropped() == 0;
    }
    if (mode == Mode::Stabilize) {
        // --test-stabilize (docs/picture.md, "Stabilising the window"), on the shaken fixture: the white disc's place on screen,
        // picture by picture, as the view shows it. Jitter is its distance from its own moving mean over a second (what a pan leaves
        // of it is not jitter). As it comes the shake is on screen in full; stabilised (once the pictures queued before the command
        // have gone, and the low-pass has settled) at most 2.5 px rms, with the pan through. Alt+S turns it off again. No drop.
        const auto& r = player.SteadyRecord();
        const auto& view = player.Renderer();
        const auto jitter = [&](std::int64_t from, std::int64_t to, unsigned& count) {
            double sum = 0; count = 0;
            for (unsigned i = 0; i < view.SpotCount(); ++i) {
                const auto& s = view.SpotAt(i);
                if (s.pts < from || s.pts > to) continue;
                double mx = 0, my = 0; unsigned n = 0;
                for (unsigned k = 0; k < view.SpotCount(); ++k) {
                    const auto& o = view.SpotAt(k);
                    if (std::llabs(o.pts - s.pts) <= 5000000) { mx += o.x; my += o.y; ++n; }
                }
                sum += std::pow(s.x - mx / n, 2) + std::pow(s.y - my / n, 2); ++count;
            }
            return count ? std::sqrt(sum / count) : 0.0;
        };
        unsigned before = 0, steadied = 0, after = 0;
        const double shaken = jitter(5000000, r.on_at - 5000000, before), steady = jitter(r.on_at + 15000000, r.off_at - 5000000, steadied);
        const double released = jitter(r.off_at + 10000000, INT64_MAX, after);
        const bool right = shaken > 10 && steady <= 2.5 && released > 10 && steadied >= 20 && before >= 20 && after >= 10;
        std::printf("Stabilize: as it comes %.2f px rms over %u pictures; stabilised %.2f px rms over %u (from %.2f s to %.2f s; %u pictures on the steadied view, %u measured); "
                    "off again %.2f px rms over %u; %u spots; commands %u, %u; %s\n", shaken, before, steady, steadied, r.on_at / 1e7, r.off_at / 1e7, r.pictures, r.measured, released,
                    after, view.SpotCount(), static_cast<unsigned>(r.status_on), static_cast<unsigned>(r.status_again), right ? "right" : "WRONG");
        complete = r.done && right && r.off_shown && r.status_on == media::ai::IntentStatus::Ok && r.status_again == media::ai::IntentStatus::NoContext &&
                   r.pictures >= 60 && r.measured >= r.pictures * 9 / 10 && player.Dropped() == 0;
    }
    if (mode == Mode::NextTarget) {
        // --test-next-target (docs/picture.md, "Several objects at once"), on the two-targets fixture: T at 1 s locks onto the
        // middle (the background between the squares); Tab on the very next picture (2026-11-01: perception's table is warm before
        // T) moves the view at once onto one square (the table's first: a move counted, the view's centre on that square, both
        // squares in the table), Tab at 2 s onto the other, "cible suivante" at 3.5 s onto the first again (two objects: "next"
        // alternates). 0.6 s after each:
        // the tracker and the view's centre on that square (within its half size and 3 samples) and the audio focus on the view,
        // the renderer's target id the id of the object on that square in perception's table, the same id as the square's at the
        // first move (the track kept for 3 s); no picture dropped.
        const auto& run = player.TargetRecord();
        const auto square = [](float x, float y, double t, double margin) {
            static constexpr double cx[2]{0.27, 0.73}, cy[2]{0.42, 0.58};
            for (int k = 0; k < 2; ++k) {
                const double h = k == 0 ? 14 * 4.0 / (4.0 - 0.45 * t) : 18 * 5.0 / (5.0 - 0.60 * t);
                if (std::fabs(x - cx[k]) * 320 <= h + margin && std::fabs(y - cy[k]) * 240 <= h + margin) return k;
            }
            return -1;
        };
        int on[4]{-1, -1, -1, -1}; bool right[4]{};
        for (unsigned a = 1; a < run.snapshots; ++a) {
            const auto& s = run.after[a];
            const double t = s.pts / 1e7;
            on[a] = s.state == 1 ? square(s.tracked_x, s.tracked_y, t, 3) : -1;
            int object = -1; // The table's object carrying the target id: on the same square.
            for (unsigned i = 0; i < s.objects; ++i) if (s.ids[i] == s.target_id) object = square(s.x[i], s.y[i], t, 3);
            right[a] = on[a] >= 0 && object == on[a] && square(s.shown_x, s.shown_y, t, 3) == on[a] && std::fabs(s.audio_x - s.shown_x) <= 0.02;
            std::printf("Next target, action %u at %.2f s: tracker %s (%.3f, %.3f) on %s; view (%.3f, %.3f) zoom %.1f, audio focus %.3f; target id %u (its object on %s); %u objects:",
                        a, t, s.state == 1 ? "tracking" : s.state == 2 ? "LOST" : "OFF", s.tracked_x, s.tracked_y, on[a] == 0 ? "A" : on[a] == 1 ? "B" : "neither",
                        s.shown_x, s.shown_y, s.zoom, s.audio_x, s.target_id, object == 0 ? "A" : object == 1 ? "B" : "neither", s.objects);
            for (unsigned i = 0; i < s.objects; ++i) std::printf(" [%u (%.2f, %.2f)]", s.ids[i], s.x[i], s.y[i]);
            std::printf("%s\n", right[a] ? "" : "  <- WRONG");
        }
        const int tab_on = square(run.tab_x, run.tab_y, 1.0, 3);
        // S at 4.2 s: the snapshot and its sidecar, which names the target followed then and marks its object.
        bool shot = false;
        if (run.snapshot_written) {
            char sidecar[520]; std::string text;
            if (media::video::SidecarPath(run.snapshot_path, sidecar, sizeof(sidecar))) {
                std::FILE* f = nullptr;
                if (fopen_s(&f, sidecar, "rb") == 0 && f) { char chunk[4096]; for (std::size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) text.append(chunk, n); std::fclose(f); }
                char target[64]; std::snprintf(target, sizeof(target), "\"target_track_id\": %u,", run.snapshot_target);
                shot = run.snapshot_target && text.find(target) != std::string::npos && text.find("\"target\": true") != std::string::npos &&
                       text.find("\"zoom\": 2.000") != std::string::npos && run.snapshot_width > 0 && run.snapshot_height > 0;
                (void)std::remove(sidecar);
            }
            (void)std::remove(run.snapshot_path);
        }
        std::printf("Next target: S at 4.2 s %s: %s (%ux%u), the sidecar's target %u %s\n", run.snapshot_pressed ? "pressed" : "NOT PRESSED",
                    run.snapshot_written ? run.snapshot_path : "NOT WRITTEN", run.snapshot_width, run.snapshot_height, run.snapshot_target, shot ? "right" : "WRONG");
        // The snapshot's readback (2026-11-02): none held while playing, before S or once the files were written; the one made
        // for it no larger than the window (its rows padded to 256 bytes).
        const std::uint64_t window_bytes = ((std::uint64_t{run.window_width} * 4 + 255) / 256 * 256) * run.window_height;
        const bool slim = run.readback_before == 0 && run.readback_after == 0 && run.readback_peak > 0 && run.readback_peak <= window_bytes;
        std::printf("Next target: snapshot readback %llu bytes before S, %llu at most (window %ux%u: %llu), %llu after %s\n",
                    static_cast<unsigned long long>(run.readback_before), static_cast<unsigned long long>(run.readback_peak), run.window_width, run.window_height,
                    static_cast<unsigned long long>(window_bytes), static_cast<unsigned long long>(run.readback_after), slim ? "(none held while playing)" : "WRONG");
        std::printf("Next target: T after %u warm grids; the first Tab %.3f s of media after T: %s, the view at once at (%.3f, %.3f) on %s, %u objects in the table\n",
                    run.warm_grids, run.tab_delay / 1e7, run.tab_moved ? "moved" : "DID NOT MOVE", run.tab_x, run.tab_y, tab_on == 0 ? "A" : tab_on == 1 ? "B" : "neither",
                    run.tab_objects);
        std::printf("Next target: %u actions, %u snapshots, \"cible suivante\" %d (0: done), %u moves; %u dropped\n", run.actions, run.snapshots, run.command, run.retargets, player.Dropped());
        complete = run.snapshots == 4 && run.command == 0 && run.retargets == 3 && right[1] && right[2] && right[3] && on[1] != on[2] && on[3] == on[1] &&
                   run.after[3].target_id == run.after[1].target_id && player.Dropped() == 0 &&
                   run.warm_grids > 0 && run.tab_delay > 0 && run.tab_delay <= 500000 && run.tab_moved && run.tab_objects >= 2 && tab_on == on[1] && shot && slim;
    }
    if (mode == Mode::Saccade) {
        // --test-saccade (docs/audio-dsp.md, "Sound localisation and the audio saccade"): "look at sound" at 1 s; the burst on
        // the right at 2 s turns the view right (2x, the view's centre at the picture's right edge less half a view), the one on
        // the left at 4 s turns it left; each shown at once through the pictures already converted (live zoom).
        const auto& s = player.SaccadeRecord();
        std::printf("Saccade: \"look at sound\" %s; right burst: azimuth %+.2f, the view at %.3f; left burst: azimuth %+.2f, the view at %.3f (zoom %.1f); %u saccades, %u dropped\n",
                    s.command == 0 ? "on" : "REFUSED", s.right_azimuth, s.right_x, s.left_azimuth, s.left_x, s.zoom, s.saccades, player.Dropped());
        complete = s.command == 0 && s.saccades == 2 && s.right_azimuth > 0.8f && s.left_azimuth < -0.8f && s.right_x > 0.7f && s.left_x >= 0 && s.left_x < 0.3f &&
                   s.zoom > 1.9f && player.Dropped() == 0;
    }
    if (mode == Mode::ZoomLive) {
        // --test-zoom-live (docs/picture.md, "Live zoom"): every change made while playing was on screen within a display refresh
        // or so (through the pictures already converted, sampled at the new region), then natively from the new region's own
        // samples; the pictures queued at the old zoom were shown through the change, not dropped or shown at the old zoom.
        const auto& r = player.Renderer();
        const double refresh = static_cast<double>(r.RefreshPeriod());
        bool good = r.LiveZooms() == 4;
        unsigned bridged = 0;
        for (unsigned i = 0; i < r.LiveZooms() && i < media::render::D3D12Renderer::kMaxLiveZooms; ++i) {
            const auto& z = r.LiveZoomAt(i);
            std::printf("Live zoom %u: on screen %.2f ms after the change (%.2f refreshes), %u images through the change, native %.1f ms after; region (%u,%u)-(%u,%u)\n",
                        i + 1, z.shown / 1e4, z.shown / refresh, z.bridged, z.native / 1e4, z.region.left, z.region.top, z.region.right, z.region.bottom);
            good = good && z.shown > 0 && z.shown <= static_cast<std::int64_t>(2 * refresh) && z.native >= z.shown;
            bridged += z.bridged;
        }
        std::printf("Live zoom: %u images shown through a change (%u in all), %u dropped\n", bridged, r.TransformedComposites(), player.Dropped());
        complete = good && bridged > 0 && r.TransformedComposites() > 0 && player.Dropped() == 0 && r.Zoom() == 1.0f;
    }
    if (mode == Mode::AbLoop) {
        // --test-ab-loop (docs/seeking.md, "A–B loop"): L, L: the loop [1.0 s, 2.0 s] (each end where the clock was), three turns,
        // each seen at B (not past it by more than a few milliseconds) and landing on the picture showing A; L: off. Then
        // "loop this sentence": the cue on screen, padded 150 ms, three turns; "בטל לופ": off, the bracket gone.
        const auto& l = player.LoopRun();
        bool good = l.done && l.count >= 6;
        for (unsigned i = 0; i < l.count && i < media::engine::PlaybackCoordinator::kMaxTurns; ++i) {
            const auto& t = l.turns[i];
            const bool exact = t.landed && t.pts <= t.a && t.a < t.pts + t.duration;
            std::printf("Loop turn %u%s: [%.4f s, %.4f s] B seen at %.4f s (%.2f ms past), landed at %.4f s in %.2f ms %s%s\n", i + 1, t.cue ? " (sentence)" : "",
                        t.a / 1e7, t.b / 1e7, t.seen / 1e7, (t.seen - t.b) / 1e4, t.pts / 1e7, t.latency / 1e4, exact ? "exact" : "WRONG",
                        t.pinned ? ", through the pinned pictures" : "");
            std::printf("  sound from %.6f s (%+.3f ms from A), clock running %.2f ms after the request; the landing waited for%s\n", t.audio / 1e7,
                        (t.audio - t.a) / 1e4, t.resumed / 1e4, Waited(t.waited));
            // The sound restarts on A's sample (within one 48 kHz frame); a pinned turn's clock runs without waiting for the decoder.
            good = good && t.resumed > 0 && std::llabs(t.audio - t.a) <= 10000000 / 48000 + 1;
            good = good && exact && t.seen >= t.b && t.seen - t.b <= 200000;
            // The pinned turnaround (docs/seeking.md): every later turn of a loop shows A from its pins, within 5 ms.
            if (i == 1 || i == 2 || i == 4 || i == 5) good = good && t.pinned && t.latency < 50000;
            if (i < 3) good = good && !t.cue && t.a >= 10000000 && t.a < 10500000 && t.b >= 20000000 && t.b < 20500000;
            else if (i < 6) {
                const auto a = l.cue_start > 1500000 ? l.cue_start - 1500000 : 0;
                good = good && t.cue && t.a == a && t.b == l.cue_end + 1500000;
            }
        }
        std::printf("Loop sentence: cue %u [%.3f s, %.3f s]; bracket after the end %08x, loop %s\n", l.cue_index + 1, l.cue_start / 1e7, l.cue_end / 1e7,
                    player.Renderer().HudState().loop, player.Loop().Looping() ? "STILL ON" : "off");
        // The landings staged ahead of B (2026-11-02): every pinned turn found its landing's copy run.
        std::printf("Loop staging: %u landings staged, %u of %u pinned turns took one\n", player.Renderer().StagedLandings(), player.Renderer().StagedTurns(),
                    player.Renderer().PinnedTurns());
        good = good && player.Renderer().PinnedTurns() >= 4 && player.Renderer().StagedTurns() == player.Renderer().PinnedTurns();
        complete = good && !player.Loop().Looping() && player.Renderer().HudState().loop == 0 && player.Dropped() == 0;
    }
    if (mode == Mode::Telemetry) {
        // --test-telemetry (docs/session.md, "Telemetry readout"): every readout is the engine's own state at that moment; the
        // explanation names the tier the governor left and the very p95 it measured; the flight recorders hold the change.
        const auto& t = player.TelemetryRun();
        bool good = t.done && t.count >= 5;
        bool explained = false, panels = false;
        for (unsigned i = 0; i < t.count && i < media::engine::PlaybackCoordinator::kMaxReadouts; ++i) {
            const auto& r = t.readouts[i];
            std::string shown(r.text); for (auto& c : shown) if (c == '\n') c = '|';
            std::printf("Readout %u (%s): %s\n", i + 1, r.explain ? "why" : "panel", shown.c_str());
            const std::string_view text{r.text};
            char tier[96]; std::snprintf(tier, sizeof(tier), "Quality \xc2\xb7 %s", media::render::Name(r.snapshot.tier));
            good = good && text.find(tier) == 0;
            if (r.explain && r.snapshot.changes) {
                const auto& c = r.snapshot.last_change;
                char measured[48]; const auto tenths = (c.p95 + 500) / 1000;
                std::snprintf(measured, sizeof(measured), "%s needed %lld.%lld ms", media::render::Name(c.from), static_cast<long long>(tenths / 10), static_cast<long long>(tenths % 10));
                const bool names = c.reason == media::render::QualityGovernor::Reason::Overload && text.find(measured) != text.npos;
                std::printf("  the governor left %s at p95 %.2f ms (budget %.2f ms): %s\n", media::render::Name(c.from), c.p95 / 1e4, c.budget / 1e4, names ? "stated exactly" : "NOT STATED");
                explained = explained || names;
            }
            if (!r.explain) {
                char frames[64]; std::snprintf(frames, sizeof(frames), "Frames \xc2\xb7 %llu shown", static_cast<unsigned long long>(r.snapshot.presented));
                panels = panels || text.find(frames) != text.npos;
                good = good && text.find(frames) != text.npos;
            }
        }
        // The flight recorders: the tier change, as the governor recorded it.
        media::core::FlightRecord events[media::engine::PlaybackCoordinator::Recorder::kCapacity];
        const unsigned n = player.UiEvents().Snapshot(events, media::engine::PlaybackCoordinator::Recorder::kCapacity);
        bool recorded = false; const auto& g = player.Renderer().Governor();
        for (unsigned i = 0; i < n; ++i)
            if (events[i].kind == media::core::FlightEvent::TierChange && g.Changes() && events[i].c == g.ChangeAt(0).p95) recorded = true;
        std::printf("Flight recorders: %llu presentation events, %llu worker events; the first tier change %s\n",
                    static_cast<unsigned long long>(player.UiEvents().Written()), static_cast<unsigned long long>(player.WorkerEvents().Written()),
                    recorded ? "recorded with its p95" : "NOT RECORDED");
        media::core::FlightRecord merged[16], a[16], b[16];
        const unsigned m = media::core::MergeNewest(player.UiEvents(), player.WorkerEvents(), merged, 16, a, b);
        for (unsigned i = 0; i < m; ++i) { char line[160]; (void)media::ui::DescribeEvent(merged[i], 0, line, sizeof(line)); std::printf("  event +%.3f s: %s\n", (merged[i].time - merged[0].time) / 1e7, line); }
        // While it played, the system and the display were kept on (the state Windows reports when the request ends).
        const auto held = player.AwakeHeld();
        const bool awake = (held & ES_SYSTEM_REQUIRED) && (held & ES_DISPLAY_REQUIRED) && (held & ES_CONTINUOUS);
        std::printf("Staying awake: execution state held while playing %08lx (%s)\n", static_cast<unsigned long>(held), awake ? "system and display" : "NOT HELD");
        // K before I: the first panel shows the loudness line with night mode's gain.
        const std::string_view first{t.readouts[0].text};
        const bool loudness = first.find("Loudness \xc2\xb7 ") != first.npos && first.find(" LUFS") != first.npos && first.find("night ") != first.npos && player.Night();
        std::printf("Loudness in the panel: %s\n", loudness ? "with night mode's gain" : "MISSING");
        complete = good && explained && panels && recorded && !player.StatsShown() && awake && loudness;
    }
    if (mode == Mode::Bookmarks) {
        // --test-bookmarks (docs/session.md, "Bookmarks"; docs/picture.md, "Pinned | Live"): three bookmarks where they were
        // set; each jump landing on its bookmark's picture; then, paused, Pinned | Live composed of exactly the pinned frame's
        // left half and the live frame's right half (hashes of the screen's halves, before the controls).
        const auto& r = player.BookmarkRun(); const auto& b = player.FileBookmarks();
        bool good = r.done && b.Count() == 3;
        static constexpr std::int64_t kAt[3]{10000000, 25000000, 40000000};
        for (unsigned i = 0; i < 3; ++i) {
            std::printf("Bookmark %u at %.4f s (asked at %.1f s)\n", i + 1, r.added[i] / 1e7, kAt[i] / 1e7);
            good = good && r.added[i] >= kAt[i] && r.added[i] - kAt[i] < 1000000 && b.At(i) == r.added[i];
        }
        static constexpr unsigned kExpected[]{1, 2, 3, 2, 3};
        for (unsigned i = 0; i < r.jump_count; ++i) {
            const auto& j = r.jumps[i];
            const bool exact = j.landed && j.pts <= j.target && j.target < j.pts + j.duration;
            std::printf("Jump %u to bookmark %u (%.4f s): landed at %.4f s in %.2f ms %s\n", i + 1, j.number, j.target / 1e7, j.pts / 1e7, j.latency / 1e4, exact ? "exact" : "WRONG");
            good = good && exact && j.number == kExpected[i];
        }
        good = good && r.jump_count == 5;
        const auto& h = r.held; const auto& f = r.reference; const auto& sc = r.screen;
        const bool kinds = h.kind == media::render::D3D12Renderer::CaptureKind::Held && f.kind == media::render::D3D12Renderer::CaptureKind::Reference &&
                           sc.kind == media::render::D3D12Renderer::CaptureKind::Screen;
        const bool split = sc.left == f.left && sc.right == h.right && sc.width == h.width && sc.height == h.height;
        const bool distinct = f.left != h.left; // The two frames differ: the halves prove where each came from.
        std::printf("Pinned | Live: pinned %.4f s, live %.4f s; screen halves %016llx %016llx, pinned left %016llx, live right %016llx: %s%s; %u composites\n",
                    r.pinned / 1e7, r.live / 1e7, static_cast<unsigned long long>(sc.left), static_cast<unsigned long long>(sc.right),
                    static_cast<unsigned long long>(f.left), static_cast<unsigned long long>(h.right), split ? "exact" : "WRONG",
                    distinct ? "" : " (the two frames are alike)", player.Renderer().PinnedCompareComposites());
        complete = good && kinds && split && distinct && player.Renderer().PinnedCompareComposites() > 0 && player.Dropped() == 0;
    }
    if (mode == Mode::Speed) {
        // --test-speed (docs/audio-dsp.md, "Playback speed"): each speed asked for became the clock's, at that slope against
        // the wall clock; the committed PCM kept its 440 Hz tone at every speed, and its 250 ms marks sat where the clock's
        // segments put them; video was on time; the frame steps landed one picture apart, three forward and two back.
        const auto& r = player.SpeedRun();
        const float wanted[]{1.0f, 1.25f, 2.0f, 0.5f, 1.0f};
        bool good = r.done && r.phase_count == std::size(wanted);
        for (unsigned i = 0; i < r.phase_count; ++i) {
            const auto& ph = r.phases[i];
            const double wall = static_cast<double>(ph.to_now - ph.from_now), slope = wall > 0 ? static_cast<double>(ph.to_media - ph.from_media) / wall : 0;
            const bool right = i < std::size(wanted) && ph.speed == wanted[i] && (wall < 5000000 || std::fabs(slope / ph.speed - 1) < 0.02);
            std::printf("Speed phase %u: %.2fx on the clock for %.2f s, media advanced %.3f s (slope %.4f) %s\n", i + 1, static_cast<double>(ph.speed), wall / 1e7,
                        static_cast<double>(ph.to_media - ph.from_media) / 1e7, slope, right ? "ok" : "WRONG");
            good = good && right && (i == 0 || wall >= 5000000);
        }
        for (unsigned i = 0; i < r.audio_count; ++i) {
            const auto& a = r.audio[i];
            const double hz = a.windows ? a.hz_sum / a.windows : 0;
            const bool right = a.windows >= 20 && std::fabs(hz - 440) < 8 && a.worst <= 120000 && (a.marks >= 2 || a.speed == 1.0f);
            std::printf("Speed audio %.2fx: tone %.1f Hz over %u windows; %u marks, worst %.1f ms from the clock's place; %u other quiet runs %s\n",
                        static_cast<double>(a.speed), hz, a.windows, a.marks, a.worst / 1e4, a.gaps, right ? "ok" : "WRONG");
            good = good && right;
        }
        bool steps = r.step_count == 5 && r.steps[0] > r.start_pts && r.steps[1] > r.steps[0] && r.steps[2] > r.steps[1] && r.steps[3] == r.steps[1] && r.steps[4] == r.steps[0];
        const auto frame = r.steps[1] - r.steps[0];
        // One picture each: 24 fps intervals are 416,666 or 416,667 ticks (the container's 1/24 s, rounded to 100 ns).
        const auto one = [&](std::int64_t d) { return std::llabs(d - frame) <= 1; };
        steps = steps && frame > 400000 && frame < 430000 && one(r.steps[0] - r.start_pts) && one(r.steps[2] - r.steps[1]);
        std::printf("Frame steps: from %.4fs:", r.start_pts / 1e7);
        for (unsigned i = 0; i < r.step_count; ++i) std::printf(" %.4fs", r.steps[i] / 1e7);
        std::printf(" (%u landed) %s\n", player.FrameSteps(), steps ? "ok" : "WRONG");
        complete = good && steps && player.Dropped() == 0 && player.FrameSteps() == 5;
    }
    if (mode == Mode::NeuralReuse) {
        // The same first 5 s of the file three times through the neural pass: reuse Off, Exact, Tolerant. Off reuses
        // nothing; tolerant matching reuses at least what exact matching does, for no more GPU time than Off.
        // Judged against the Off phase, which runs under the same conditions (2026-11-03; docs/neural.md, "On a shared iGPU"):
        // the iGPU is shared, and with a browser decoding video on it every phase drops whatever its GPU time (timestamps
        // leave out the other process's work), so a fixed budget no longer says whether a phase "fits". What reuse must show:
        // the tiles it reuses (the content's: 17 % exact, 35 % tolerant), GPU time per conversion under Off's (tolerant at most
        // 0.95 of it: 0.83-0.91 measured quiet, 0.87-0.90 under a browser's video), and drops: tolerant none, or fewer than
        // Off under the same load (Off 98 %, tolerant 32-72 % with a browser's video and a hardware decode at once); exact at
        // most Off's rate and 5 points.
        const auto& run = player.NeuralReuse();
        const char* names[]{"off", "exact", "tolerant"};
        double mean[3]{}, share[3]{};
        for (unsigned i = 0; i < 3; ++i) {
            const auto& p = run.phase[i];
            const auto tiles = p.reused + p.evaluated;
            share[i] = tiles ? 100.0 * p.reused / tiles : 0; mean[i] = p.gpu_count ? p.gpu_sum / 1e4 / p.gpu_count : 0;
            std::printf("Neural reuse %-8s tiles reused %u of %u (%.1f %%); GPU per neural conversion %.2f ms (%u conversions); pictures %u shown, %u dropped (%u/%u/%u/%u within 0.5/1/2/3 s of the landing)\n",
                        names[i], p.reused, tiles, share[i], mean[i], p.gpu_count, p.presented, p.dropped, p.dropped_by[0], p.dropped_by[1], p.dropped_by[2], p.dropped_by[3]);
        }
        // Without reuse the pass does not fit 720p60 on the reference GPU (docs/neural.md): that phase may drop pictures.
        const double fits = media::render::GovernorOptions{}.step_down * static_cast<double>(player.Renderer().Governor().Budget()) / 1e4;
        const auto rate = [&](unsigned i) { const auto& p = run.phase[i]; return p.presented + p.dropped ? static_cast<double>(p.dropped) / (p.presented + p.dropped) : 0.0; };
        bool phases = true;
        for (unsigned i = 1; i < 3; ++i) {
            const bool cheaper = i == 1 ? mean[1] <= mean[0] : mean[2] <= 0.95 * mean[0];
            const bool fewer = i == 1 ? rate(1) <= rate(0) + 0.05 : run.phase[2].dropped == 0 || rate(2) < rate(0);
            std::printf("Neural reuse %-8s GPU %.2f of off's %.2f ms (%.2fx; budget %.2f ms), drop rate %.1f %% against off's %.1f %%: %s\n", names[i], mean[i], mean[0],
                        mean[0] > 0 ? mean[i] / mean[0] : 0.0, fits, 100 * rate(i), 100 * rate(0), cheaper && fewer ? "ok" : !cheaper ? "NOT CHEAPER" : "TOO MANY DROPS");
            phases = phases && cheaper && fewer;
        }
        complete = player.OutOfOrder() == 0 && player.Presented() > 0 && run.done && run.phase[0].reused == 0 && run.phase[0].evaluated > 0 &&
                   share[1] >= 15 && share[2] >= 30 && share[2] >= share[1] && run.phase[2].gpu_count > 0 && fits > 0 && phases;
    }
    if (mode == Mode::FollowUps) {
        // Every follow-up became the action it refers to (its kind, its outcome, what was said) and each seek landed exactly
        // on its target with the target's captions; the comparison was converted while on and is off at the end.
        const auto cues = player.Captions().Cues();
        bool good_all = player.CommandResults() == std::size(kFollowUpExpected);
        for (unsigned i = 0; i < player.CommandResults() && i < std::size(kFollowUpExpected); ++i) {
            const auto& r = player.CommandAt(i); const auto& e = kFollowUpExpected[i];
            bool good = r.intent.kind == e.kind && r.intent.status == e.status && r.intent.from == e.from;
            const bool seek = e.status == IntentStatus::Ok && (e.kind == IntentKind::FindSubtitle || e.kind == IntentKind::SeekRelative ||
                                                                e.kind == IntentKind::SeekAbsolute || e.kind == IntentKind::ReplayCue);
            std::int64_t target = -1;
            if (seek) {
                target = e.cue >= 0 ? cues[static_cast<unsigned>(e.cue)].start : r.intent.target_100ns;
                if (e.cue == -2) { target = -1; for (const auto& cue : cues) if (r.pts <= cue.start && cue.start < r.pts + r.duration) target = cue.start; }
                const bool exact = r.landed && target >= 0 && r.pts <= target && target < r.pts + r.duration;
                bool quoted = e.cue < 0;
                for (unsigned k = 0; k < r.subtitles.count; ++k) quoted = quoted || r.subtitles.cues[k] == static_cast<unsigned>(e.cue);
                good = good && exact && quoted && CaptionsMatch(player, r.subtitles, Landing(target, r.pts, r.duration));
            } else good = good && r.epoch == 0;
            std::printf("Follow-up %u \"%s\": %s %s from %s match %u", i + 1, kFollowUpScript[i].text, media::ai::Name(r.intent.kind),
                        media::ai::Name(r.intent.status), media::ai::Name(r.intent.from), r.intent.match);
            if (seek) std::printf("; target=%.4fs landed=%.4fs", target / 1e7, r.pts / 1e7);
            std::printf("%s\n", good ? "" : "  UNEXPECTED");
            good_all = good_all && good;
        }
        const auto& stats = player.CommandStats(); const auto& rr = player.Renderer(); const auto& conversation = player.Conversation();
        std::printf("Follow-ups: %u commands, %u executed, %u refused, %u failed; seeks=%u; quote \"%.*s\" match %u; comparison %u pictures, %s at the end\n",
                    stats.commands, stats.executed, stats.refused, stats.failed, player.Seeks(), static_cast<int>(conversation.Query().size()),
                    conversation.Query().data(), conversation.Match(), rr.ComparisonFrames(), rr.Comparison() ? "on" : "off");
        complete = complete && good_all && stats.commands == 11 && stats.executed == 10 && stats.refused == 1 && stats.failed == 0 && player.Seeks() == 8 &&
                   conversation.Query() == "\xc2\xb7" && conversation.Match() == 1 && rr.ComparisonFrames() > 0 && !rr.Comparison();
    }
    if (mode == Mode::Intents || mode == Mode::Palette) {
        // Every command became the expected typed action (or was refused for the expected reason) and left the engine
        // in the expected state; each seek landed on the frame on screen at its target, shown with the target's cues.
        const auto cues = player.Captions().Cues();
        const auto* expected = mode == Mode::Intents ? kIntentExpected : kPaletteExpected;
        const auto expected_count = mode == Mode::Intents ? std::size(kIntentExpected) : std::size(kPaletteExpected);
        complete = complete && player.CommandResults() == expected_count;
        std::int64_t resolve[media::engine::PlaybackCoordinator::kMaxCommands]{}; unsigned timed = 0;
        for (unsigned i = 0; i < player.CommandResults() && i < expected_count; ++i) {
            const auto& r = player.CommandAt(i); const auto& e = expected[i];
            bool good = r.intent.kind == e.kind && r.intent.status == e.status && r.volume == e.volume && r.muted == e.muted && r.paused == e.paused;
            const bool seek = e.status == IntentStatus::Ok && (e.kind == IntentKind::FindSubtitle || e.kind == IntentKind::SeekRelative);
            if (seek) {
                const auto target = e.cue >= 0 ? cues[static_cast<unsigned>(e.cue)].start : r.intent.target_100ns;
                const bool exact = r.landed && r.pts <= target && target < r.pts + r.duration;
                good = CaptionsMatch(player, r.subtitles, Landing(target, r.pts, r.duration)) && good && exact;
                // A quote search shows the quoted cue on its landing frame.
                bool quoted = e.cue < 0;
                for (unsigned k = 0; k < r.subtitles.count; ++k) quoted = quoted || r.subtitles.cues[k] == static_cast<unsigned>(e.cue);
                good = good && quoted;
                std::printf("Command %u seek: target=%.4fs landed=%.4fs %s\n", i + 1, target / 1e7, r.pts / 1e7, exact && quoted ? "exact" : "WRONG");
            } else good = good && r.epoch == 0;
            if (!good) std::printf("Command %u: UNEXPECTED\n", i + 1);
            complete = complete && good; resolve[timed++] = r.resolve;
        }
        // From the text to the engine action (parse, validate, look up, enqueue). The median is asserted under a
        // millisecond: one measurement on a desktop system also contains whatever else the scheduler ran meanwhile.
        std::sort(resolve, resolve + timed);
        if (timed) std::printf("Command time: median %.1fus, slowest %.1fus\n", resolve[timed / 2] / 10.0, resolve[timed - 1] / 10.0);
        complete = complete && timed && resolve[timed / 2] < 10000;
        const auto& stats = player.CommandStats(); const auto& g = player.GainStats();
        std::printf("Intents: %u commands, %u executed, %u refused, %u failed; seeks=%u; volume pill composites=%u; gain ramps=%u\n", stats.commands,
                    stats.executed, stats.refused, stats.failed, player.Seeks(), player.Renderer().VolumePresents(), g.ramps);
        if (mode == Mode::Intents)
            complete = complete && stats.commands == 12 && stats.executed == 10 && stats.refused == 2 && stats.failed == 0 && player.Seeks() == 3 &&
                       player.Volume() == 0.45f && !player.Muted() && !player.Paused() && player.Renderer().VolumePresents() > 10 && g.ramps >= 4;
        else {
            // Typed through the window: nine Enter presses, one of them refused (the quote nobody says) and dismissed with
            // Escape; the bar was drawn with its rasterised text while typing, and is closed and gone at the end.
            const auto& r = player.Renderer(); const auto& text = r.PaletteText().Stats();
            std::printf("Command bar: %u submits, %u refused; %u composites with the bar; %u text rasters, %u shown, %u failures; open at the end: %u\n",
                        r.PaletteSubmits(), r.PaletteRejections(), r.PalettePresents(), text.rasters, text.uploads, text.failures, r.Palette().IsOpen() ? 1u : 0u);
            complete = complete && stats.commands == 9 && stats.executed == 8 && stats.refused == 0 && stats.failed == 1 && player.Seeks() == 3 &&
                       player.Volume() == 0.40f && !player.Muted() && !player.Paused() && r.PaletteSubmits() == 9 && r.PaletteRejections() == 1 &&
                       r.PalettePresents() > 60 && text.rasters >= 9 && text.uploads >= 9 && text.failures == 0 && !r.Palette().IsOpen() && !r.Palette().Visible();
        }
    }
    if (player.Tracks() > 2 || mode == Mode::Tracks) {
        // The catalogue: every track of the file, how the engine can use it.
        static const char* const kinds[]{"other", "video", "audio", "subtitles"};
        static const char* const uses[]{"selectable", "refused", "undecodable", "incompatible", "chapters"};
        for (unsigned i = 0; i < player.Tracks(); ++i) {
            const auto& t = player.TrackAt(i);
            const char entry[5]{static_cast<char>(t.sample_entry >> 24), static_cast<char>(t.sample_entry >> 16), static_cast<char>(t.sample_entry >> 8), static_cast<char>(t.sample_entry), 0};
            std::printf("Track %u: %s %s '%s' language=%s%s%s%s %u ch %u Hz group %u%s\n", t.id, kinds[static_cast<unsigned>(t.kind) & 3], uses[(std::min)(static_cast<unsigned>(t.use), 4u)],
                        t.sample_entry ? entry : "", t.language[0] ? t.language : "-", t.name[0] ? " name=\"" : "", t.name, t.name[0] ? "\"" : "",
                        t.channels, t.sample_rate, t.alternate_group, t.enabled ? " default" : "");
        }
        std::printf("Embedded subtitles: read in %.2f ms\n", player.SubtitleImportTime() / 1e4);
    }
    if (mode == Mode::Tracks || mode == Mode::TracksSurround) {
        using media::engine::TrackUse;
        const bool from_surround = mode == Mode::TracksSurround;
        const auto* script = from_surround ? kTrackSurroundScript : kTrackScript;
        const auto* expected = from_surround ? kTrackSurroundExpected : kTrackExpected;
        const unsigned expected_count = from_surround ? static_cast<unsigned>(std::size(kTrackSurroundExpected)) : static_cast<unsigned>(std::size(kTrackExpected));
        // 1. The catalogue as the fixture was written.
        const TrackUse want[]{TrackUse::Selectable, TrackUse::Selectable, TrackUse::Selectable, TrackUse::Selectable, TrackUse::Refused,
                              TrackUse::Selectable, TrackUse::Selectable, TrackUse::Refused, TrackUse::Selectable};
        bool catalogued = player.Tracks() == std::size(want);
        for (unsigned i = 0; catalogued && i < std::size(want); ++i) catalogued = player.TrackAt(i).use == want[i] && player.TrackAt(i).id == i + 1;
        // The Arabic track's sample entry says 2 channels; its AudioSpecificConfig says 5.1, and that is what is listed.
        catalogued = catalogued && player.TrackAt(8).channels == 6 && player.TrackAt(3).sample_rate == 44100;
        if (!catalogued) std::puts("Catalogue: UNEXPECTED");
        // 2. Every command: its intent, and the tracks chosen right after it.
        bool commands = player.CommandResults() == expected_count;
        for (unsigned i = 0; i < player.CommandResults() && i < expected_count; ++i) {
            const auto& r = player.CommandAt(i); const auto& e = expected[i];
            const bool good = r.intent.kind == e.kind && r.intent.status == e.status && r.audio_track == e.audio && r.subtitle_track == e.subtitles;
            std::printf("Command %u \"%s\": %s %s; audio %u, subtitles %s%s\n", i + 1, script[i].text, media::ai::Name(r.intent.kind), media::ai::Name(r.intent.status),
                        r.audio_track, r.subtitle_track == kNone ? "off" : std::to_string(r.subtitle_track).c_str(), good ? "" : "  UNEXPECTED");
            commands = commands && good;
        }
        // 3. The worker's changes, and what the PCM showed of each splice.
        const auto& probe = player.Probe();
        unsigned spliced = 0; bool splices = player.TrackSwitches() == (from_surround ? 4u : 5u);
        const auto tone = [](std::uint32_t id) { return id == 2 ? 440.0f : id == 3 ? 660.0f : id == 4 ? 880.0f : 550.0f; };
        for (unsigned i = 0; i < player.TrackSwitches(); ++i) {
            const auto& s = player.SwitchAt(i);
            std::printf("Switch %u: track %u -> %u (%u Hz, %u ch%s), %s at %.4f s (frame %llu), applied %.2f ms after the request; heard %.1f ms later (old audio queued)\n",
                        i + 1, s.from, s.to, s.rate, s.channels, s.converted ? ", converted" : "", s.spliced ? "spliced" : "cold", s.at / 1e7,
                        static_cast<unsigned long long>(s.frame), (s.applied - s.requested) / 1e4, s.queued / 1e4);
            // French (44.1 kHz stereo) and Arabic (48 kHz 5.1) are converted to the English track's 48 kHz stereo; started
            // on Arabic, every stereo track is converted to its 5.1 (and French to 48 kHz too).
            const bool convert = from_surround ? s.to != 9 : s.to == 4 || s.to == 9;
            splices = splices && s.converted == convert && s.rate == (s.to == 4 ? 44100u : 48000u) && s.channels == (s.to == 9 ? 6u : 2u);
            if (!s.spliced) continue;
            const float want_before = tone(s.from), want_after = tone(s.to);
            if (spliced < probe.splice_count) {
                const auto& p = probe.splices[spliced];
                const bool tones = std::abs(p.before_hz - want_before) < 40 && std::abs(p.after_hz - want_after) < 40 && p.frame == s.frame;
                std::printf("  PCM: %.0f Hz before, %.0f Hz after, quietest 1 ms at %.2f of the tone%s\n", p.before_hz, p.after_hz, p.dip, tones ? "" : "  UNEXPECTED");
                splices = splices && tones;
            }
            ++spliced;
        }
        // The first four changes are made while PCM flows; the fifth comes 30 ms behind a seek (either way is right).
        splices = splices && spliced >= 4 && probe.splice_count == spliced && player.SwitchAt(0).spliced && player.SwitchAt(1).spliced && player.SwitchAt(2).spliced &&
                  player.SwitchAt(3).spliced;
        if (from_surround) std::printf("Audio path: %u decoded channels, %u to the endpoint, fold-down %s\n", player.Routing().decoded_channels, player.Routing().output_channels,
                                  player.Routing().fold_down ? "yes" : "no");
        splices = splices && (!from_surround || player.Routing().decoded_channels == 6);
        std::printf("Time marks: %u found, worst %.3f ms off the first one's place in the 250 ms grid; %u unexpected silences\n", probe.notches,
                    probe.worst_error / 1e4, probe.gaps);
        const bool aligned = probe.notches >= 30 && probe.worst_error <= 10000 && probe.gaps == 0;
        // 4. Subtitles: the Hebrew channel is the one shown at the end; captions were composited.
        const auto* hebrew = player.FindTrack(6, media::demux::TrackKind::Subtitle);
        const bool captions = hebrew && player.SubtitleTrackId() == 6 && player.Captions().Shown() == (1u << hebrew->slot) && player.SubtitlePresents() > 0 &&
                              player.Captions().Channels() == 2;
        // 5. Status cues: each change and refusal posted one, and they reached the screen with their text.
        const auto& renderer = player.Renderer();
        std::printf("Status cues: %u shown with their text, %u composites with the pill, %u rasters (%u failures); last: \"%.*s\"\n", renderer.StatusShown(),
                    renderer.StatusPresents(), renderer.StatusText().Stats().rasters, renderer.StatusText().Stats().failures,
                    static_cast<int>(renderer.Status().Text().size()), renderer.Status().Text().data());
        const bool cues = renderer.StatusShown() >= (from_surround ? 4u : 8u) && renderer.StatusPresents() > 20 && renderer.StatusText().Stats().failures == 0;
        complete = complete && catalogued && commands && splices && aligned && (captions || from_surround) && cues && !player.Paused() && player.Presented() > 200;
    }
    if (fixture) {
        // At least 60 pictures and two seconds of decoded audio, every picture on time and in order.
        const auto& routing = player.Routing();
        std::printf("Fixture: %u pictures shown, %u decoded as references only, %u neither shown nor referenced and not decoded; %.2f s of audio at %u Hz, %u samples indexed in %llu bytes\n",
                    player.Presented(), player.Hidden(), player.Skipped(),
                    player.AudioRate() ? static_cast<double>(player.AudioFrames()) / player.AudioRate() : 0.0, player.AudioRate(),
                    static_cast<unsigned>(player.IndexedSamples()), static_cast<unsigned long long>(player.IndexBytes()));
        complete = player.Presented() >= 60 && player.Dropped() == 0 && player.OutOfOrder() == 0 &&
                   routing.status == media::engine::AudioStatus::Ready && player.AudioFrames() >= 2 * player.AudioRate();
    }
    if (mode == Mode::FixtureHdr) {
        // The initial preview records the first picture's duration; nothing seeks in this mode.
        const auto& r = player.Renderer();
        const auto interval = r.LastSeek().duration, last = r.ShownPts();
        const auto expected = interval > 0 ? static_cast<std::int64_t>(player.Presented() - 1) * interval : -1;
        const double seconds = (wall_ms - player.ClockStart() / 1e4) / 1e3;
        std::printf("HDR fixture: %ux%u %s, %u of %u pictures tone-mapped, frame interval %.3fms, last picture at %.4fs (%.4fs if none was skipped), %.1f pictures per second of clock\n",
                    player.Width(), player.Height(), player.TenBit() ? "10-bit" : "8-bit", player.ToneMapped(), player.Presented(), interval / 1e4,
                    last / 1e7, expected / 1e7, seconds > 0 ? player.Presented() / seconds : 0.0);
        // The pre-roll absorbs the first Convert + Present pair (a cold driver's first conversion, the swap chain's first flip):
        // the clock starts only once the first picture is on its way to the screen, so the strict count below begins after it.
        std::printf("HDR fixture: first picture presented %.1f ms after Run, the clock started %.1f ms after Run (%s)\n", player.FirstFrameTime() / 1e4,
                    player.ClockStart() / 1e4, player.FirstFrameTime() > 0 && player.FirstFrameTime() <= player.ClockStart() ? "after it" : "BEFORE IT");
        complete = complete && player.Hevc() && player.TenBit() && player.Width() >= 3840 && player.Height() >= 2160 &&
                   player.FirstFrameTime() > 0 && player.FirstFrameTime() <= player.ClockStart() &&
                   player.ToneMapped() >= player.Presented() && interval > 0 && interval <= 200000 && player.Presented() >= 150 &&
                   last >= expected - interval / 2 && last <= expected + interval / 2 && r.WorstLateness() <= interval;
    }
    if (mode == Mode::FixtureSearch) {
        // Each quote was found in the expected cue and its seek landed on the frame on screen at that cue's start,
        // composited with the cue; the two typed ones went through the command bar.
        const auto cues = player.Captions().Cues(); const auto& st = player.Renderer().Subtitles().Stats();
        complete = complete && player.CommandResults() == std::size(kFixtureCues) && player.SubtitleState() == media::render::SubtitleStatus::Ready;
        for (unsigned i = 0; i < player.CommandResults() && i < std::size(kFixtureCues); ++i) {
            const auto& r = player.CommandAt(i); const unsigned cue = kFixtureCues[i];
            bool good = cue < cues.size() && r.intent.kind == IntentKind::FindSubtitle && r.intent.status == IntentStatus::Ok && r.landed;
            if (good) {
                const auto target = cues[cue].start;
                bool quoted = false;
                for (unsigned k = 0; k < r.subtitles.count; ++k) quoted = quoted || r.subtitles.cues[k] == cue;
                good = r.pts <= target && target < r.pts + r.duration && quoted && CaptionsMatch(player, r.subtitles, Landing(target, r.pts, r.duration));
                // Concept matches say so, with the line they found; quotes found as typed do not.
                const bool meant = kFixtureConcepts[i];
                good = good && r.by_concept == meant && (!meant || std::strncmp(r.status, "Concept match: \"", 16) == 0);
                std::printf("Quote %u: cue %u at %.3fs, landed=%.4fs in %.1fms %s%s%s\n", i + 1, cue, target / 1e7, r.pts / 1e7, r.latency / 1e4, good ? "exact" : "WRONG",
                            meant ? "; cue: " : "", meant ? r.status : "");
            } else std::printf("Quote %u: NOT FOUND\n", i + 1);
            complete = complete && good;
        }
        complete = complete && player.Seeks() == std::size(kFixtureCues) && player.Renderer().PaletteSubmits() == 3 &&
                   player.Renderer().PaletteRejections() == 0 && player.SubtitlePresents() > 30 && st.generated > 0;
    }
    if (mode == Mode::Underrun) complete = complete && player.SilenceFrames() > 0 && player.Discontinuities() >= 1;
    if (mode == Mode::Hiccup) {
        // --test-hiccup (docs/session.md, "The audio lead"): two short gaps in the stream (10 and 15 ms of silence), each bridged:
        // the clock never stops (no re-anchor, no hold on the pictures), nothing dropped.
        std::printf("Hiccups: %u bridged, %u re-anchors, %llu frames of silence, %u pictures dropped\n", player.BridgedGaps(), player.Discontinuities(),
                    static_cast<unsigned long long>(player.SilenceFrames()), player.Dropped());
        complete = player.BridgedGaps() == 2 && player.Discontinuities() == 0 && player.Dropped() == 0 && player.SilenceFrames() > 0 && player.Presented() >= 400;
    }
    // Pause: the endpoint stops, the clock re-anchors on resume, and playback lasts at least the pause longer.
    if (mode == Mode::Pause) complete = complete && player.Discontinuities() >= 1 && wall_ms >= 3000 + 700;
    if (mode == Mode::Attach) {
        const unsigned presents = player.SubtitlePresents();
        std::printf("Attach: first %d (captions drawn %u before, %u after it), its cue live %s; a file without a cue %d (refused); second %d, its cue live %s; "
                    "%u captioned presents in all\n", attach.first, attach.presents_before, attach.presents_first, attach.first_shown ? "yes" : "NO", attach.empty,
                    attach.second, attach.second_shown ? "yes" : "NO", presents);
        using R = media::engine::PlaybackCoordinator::AttachResult;
        complete = complete && attach.first == static_cast<int>(R::Ok) && attach.presents_before == 0 && attach.presents_first > 10 && attach.first_shown &&
                   attach.empty == static_cast<int>(R::Unreadable) && attach.second == static_cast<int>(R::Ok) && attach.second_shown &&
                   presents > attach.presents_first + 10 && player.Dropped() == 0 && player.OutOfOrder() == 0 && player.Presented() > 100;
    }
    if (mode == Mode::Pacing) {
        // Both states measured, nothing dropped; with low-power pacing the presentation thread waits instead of spinning.
        const auto& p = player.PacingStats();
        const double playing = p.playing.wall > 0 ? static_cast<double>(p.playing.presentation) / static_cast<double>(p.playing.wall) : 1.0;
        const double paused = p.paused.wall > 0 ? static_cast<double>(p.paused.presentation) / static_cast<double>(p.paused.wall) : 1.0;
        const auto worker = [](const media::engine::PlaybackCoordinator::Pacing::Share& s) {
            return s.wall > 0 ? static_cast<double>(s.worker) / static_cast<double>(s.wall) : 1.0;
        };
        complete = complete && player.Dropped() == 0 && player.OutOfOrder() == 0 && player.Presented() >= 100 && p.playing.wall >= 60000000 &&
                   p.paused.wall >= 35000000 && player.Discontinuities() >= 1 &&
                   (!options.low_power_pacing || (p.waits > 0 && p.worker_waits > 0 && playing < kPacingPlaying && paused < kPacingPaused &&
                                                  // A software decoder (AV1, API 17) decodes and uploads on the worker: its share is bounded, not low
                                                  // (Release 55-74 %, Debug 86 % with a browser decoding 4K on the iGPU, 2026-11-05); a spin reads 100 %.
                                                  worker(p.playing) < (player.SoftwareVideo() ? 0.95 : kPacingPlaying) && worker(p.paused) < kPacingPaused &&
                                                  p.settled_waits >= 3)); // The settled pause's long sleeps (2026-11-03: 15 in 4 s).
    }
    if (mode == Mode::Hud) {
        std::printf("HUD: presents=%u paused=%u seeks=%u rejected-seeks=%u lane=%g\n", player.HudPresents(), player.PausedHudPresents(),
                    player.Seeks(), player.RejectedSeeks(), static_cast<double>(player.HudScrubOffset()));
        // The scripted drag moves to the end while paused and is released there: a scrub seek on the move, and the release's
        // exact seek unless the scrub seek had already landed there exactly. The HUD's winding lane collapses because the
        // landed frame reached the screen (well before its 3 s commit timeout).
        complete = complete && player.HudPresents() > 30 && player.PausedHudPresents() >= 10 && player.Seeks() >= 1 && player.Seeks() <= 2 &&
                   player.RejectedSeeks() == 0 && player.Discontinuities() >= 1 && player.HudScrubOffset() == 0.0f &&
                   player.LandedEpoch() == player.Seeks() + 1 && !player.ShowingPreview();
    }
    if (options.voice != media::engine::VoiceInput::Off) {
        using media::ai::IntentKind; using media::ai::IntentStatus;
        const auto v = player.Voice();
        std::printf("Voice: %s (native %08lx), %u Hz; %llu frames in, %u utterances detected, %u handed to the grammar, %u too short, %u with no command heard;"
                    " recogniser: %u accepted (%u as sentences), %u filler, %u no command alone, %u distant, %u ambiguous\n", v.active ? "on" : "off",
                    static_cast<unsigned long>(v.error), v.sample_rate, static_cast<unsigned long long>(v.captured), v.gate.utterances, v.gate.handed, v.gate.too_short,
                    v.gate.empty, v.recogniser.accepted, v.recogniser.sentences, v.recogniser.filler, v.recogniser.partial, v.recogniser.distant, v.recogniser.ambiguous);
        for (unsigned i = 0; i < player.VoiceResults(); ++i) {
            const auto& r = player.VoiceAt(i);
            std::printf("  heard \"%s\" at %.2fs (distance %.2f): %s; %s %s; paused=%d muted=%d volume=%.2f\n", r.text, r.heard / 1e7, static_cast<double>(r.distance),
                        media::ai::Name(r.wake), media::ai::Name(r.intent.kind), media::ai::Name(r.intent.status), r.paused, r.muted, static_cast<double>(r.volume));
        }
        if (mode == Mode::Voice) {
            // Said, in order (Mark unless noted): "pause" without the name (room talk: ignored); "avion", then "pause"
            // (paused); Julie: "avion", then Hebrew "nagen" (playing); "avion", then "quieter"; "garden" (no command);
            // "avion turn up the volume a bit" in one breath; "can you please pause the movie" without the name
            // (ignored); "avion can you please pause the movie" in one breath (paused); "avion next chapter" in one breath
            // (the next synthesized section, while paused). Three listening cues and six commands, in that order.
            using media::ai::WakeDecision;
            const auto at = [&](unsigned i) -> const media::engine::PlaybackCoordinator::VoiceResult& { return player.VoiceAt(i); };
            std::printf("Wake word: %u utterances, %u commands in one breath, %u names alone, %u commands in a window, %u ignored, %u windows expired\n",
                        v.wake.utterances, v.wake.commands, v.wake.wakes, v.wake.window_commands, v.wake.ignored, v.wake.expired);
            const auto& r = player.Renderer();
            std::printf("Status cues: %u shown, %u composites with the pill, last: \"%.*s\"\n", r.StatusShown(), r.StatusPresents(),
                        static_cast<int>(r.Status().Text().size()), r.Status().Text().data());
            complete = complete && v.active && v.script_done && player.VoiceResults() == 9 && v.wake.wakes == 3 && v.wake.window_commands == 3 &&
                       v.wake.commands == 3 && v.wake.ignored == 2 &&
                       at(8).wake == WakeDecision::Command && at(8).intent.kind == IntentKind::NextChapter && at(8).intent.status == IntentStatus::Ok &&
                       player.Navigations() == 1 && player.NavigationAt(0).chapter && player.NavigationAt(0).target > player.NavigationAt(0).from &&
                       at(0).wake == WakeDecision::Listening && at(0).intent.kind == IntentKind::None && !at(0).paused &&
                       at(1).wake == WakeDecision::WindowCommand && at(1).intent.kind == IntentKind::Pause && at(1).intent.status == IntentStatus::Ok && at(1).paused &&
                       at(2).wake == WakeDecision::Listening &&
                       at(3).wake == WakeDecision::WindowCommand && at(3).intent.kind == IntentKind::Play && at(3).intent.status == IntentStatus::Ok && !at(3).paused &&
                       at(4).wake == WakeDecision::Listening &&
                       at(5).wake == WakeDecision::WindowCommand && at(5).intent.kind == IntentKind::VolumeDown && at(5).volume < at(3).volume &&
                       at(6).wake == WakeDecision::Command && at(6).intent.kind == IntentKind::VolumeUp && at(6).volume > at(5).volume &&
                       at(7).wake == WakeDecision::Command && at(7).intent.kind == IntentKind::Pause && at(7).paused && player.Paused() && !player.Muted() &&
                       r.StatusShown() >= 8;
        }
    }
    if (mode == Mode::Anamorphic) {
        // The anamorphic fixture (make_playback_fixture --anamorphic): a 60x44 crop of 64x64 coded samples, VUI sample
        // aspect 4:3 and BT.601 colour, a default 1:1 pasp and a BT.709 colr in the container. Shown as 80x44 (the VUI's
        // aspect, as the pasp says nothing), converted as BT.709 (the container's description wins); the window and
        // the picture in it have that shape, without bars.
        using namespace media::video;
        const auto& p = player.Picture(); const auto& r = player.Renderer(); const auto rect = r.LastVideoRect();
        std::printf("Anamorphic: window %ux%u, picture at (%u,%u) %ux%u, renderer sample aspect %u:%u\n", r.Width(), r.Height(), rect.x, rect.y, rect.width, rect.height,
                    r.SampleAspect().width, r.SampleAspect().height);
        complete = complete && p.coded_width == 64 && p.coded_height == 64 && p.left == 0 && p.top == 0 && p.right == 60 && p.bottom == 44 &&
                   p.bitstream_aspect == SampleAspect{4, 3} && p.container_aspect == SampleAspect{1, 1} && p.sample_aspect == SampleAspect{4, 3} &&
                   p.aspect_source == AspectSource::Bitstream && p.display == PictureSize{80, 44} &&
                   p.bitstream_color.primaries == 5 && p.bitstream_color.transfer == 6 && p.bitstream_color.matrix == 5 && p.bitstream_color.described &&
                   p.bitstream_color.range_described && !p.bitstream_color.full_range && p.container_color.described && p.container_color.matrix == 1 &&
                   p.color.primaries == 1 && p.color.transfer == 1 && p.color.matrix == 1 && !p.color.full_range &&
                   r.SampleAspect() == SampleAspect{4, 3} && r.Width() == 640 && r.Height() == 352 &&
                   rect.x == 0 && rect.y == 0 && rect.width == 640 && rect.height == 352;
    }
    // HDR: every displayable picture went through the PQ/HLG tone-mapping pass.
    if (mode == Mode::Hdr) complete = complete && player.TenBit() && player.ToneMapped() == 72;
    // SDR fixtures must never take the HDR pass.
    if (mode != Mode::Hdr && !fixture) complete = complete && player.ToneMapped() == 0;
    if (http) {
        // Streamed through several ranged GETs without a failure, and without downloading the file twice over.
        const auto n = player.Network();
        complete = complete && player.Remote() && n.failures == 0 && n.gets >= 2 && n.bytes <= 2 * served.size();
    }
    player.Close();
    if (mode == Mode::Bookmarks) {
        // Kept with the file: the ledger saved and loaded, and the file opened again with it has the same three bookmarks.
        static std::byte bytes[media::session::ResumeLedger::kFileBytes];
        static media::session::ResumeLedger loaded;
        const bool saved = bookmark_ledger.Save(bytes) && loaded.Load(bytes);
        static media::engine::PlaybackCoordinator again;
        media::engine::PlaybackOptions reopen; reopen.visible = false; reopen.activate = false; reopen.resume = &loaded;
        const bool prepared = again.Prepare(source, reopen);
        bool same = prepared && again.FileBookmarks().Count() == player.FileBookmarks().Count();
        for (unsigned i = 0; same && i < again.FileBookmarks().Count(); ++i) same = again.FileBookmarks().At(i) == player.FileBookmarks().At(i);
        std::printf("Bookmarks kept: ledger of %zu bytes %s; opened again with %u bookmark(s) %s\n", sizeof(bytes), saved ? "saved and loaded" : "NOT LOADED",
                    prepared ? again.FileBookmarks().Count() : 0u, same ? "as they were" : "DIFFERENT");
        again.Close();
        complete = complete && saved && same;
    }
    return played && complete && player.Error() == 0 && allocations == 0 && (player.Presented() != 0 || player.MusicMode()) ? 0 : 1;
}
