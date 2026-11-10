#pragma once
#include "media/demux/Demuxer.hpp"
#include "media/platform/windows/Win32VideoMftDecoder.hpp"
#include "media/platform/windows/Win32IocpBlockSource.hpp"
#include "media/platform/windows/HttpBlockSource.hpp"
#include "media/platform/windows/H264Dxva.hpp"
#include "media/platform/windows/HevcDxva.hpp"
#include "media/platform/windows/Win32WasapiAudioSink.hpp"
#include "media/platform/windows/Win32WasapiCapture.hpp"
#include "media/ai/AcousticIntentRecognizer.hpp"
#include "media/ai/CustomWakeWord.hpp"
#include "media/ai/VoiceGate.hpp"
#include "media/ai/AvionWakeGate.hpp"
#include "media/render/D3D12Renderer.hpp"
#include "media/video/TensorExport.hpp"
#include "media/audio/DspProcessor.hpp"
#include "media/audio/SoundLocator.hpp"
#include "media/audio/PcmConverter.hpp"
#include "media/audio/TimeStretcher.hpp"
#include "media/subtitle/SubtitleSearch.hpp"
#include "media/ai/ConceptSearch.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/video/ColorDescription.hpp"
#include "media/video/PictureGeometry.hpp"
#include "media/timeline/ChapterTimeline.hpp"
#include "media/timeline/LoopRegion.hpp"
#include "media/ui/TelemetryReadout.hpp"
#include "media/core/flight_recorder.hpp"
#include "media/session/MediaSession.hpp"
#include "media/engine/EngineModules.hpp"
#include "media/engine/FrameSignature.hpp"
#include "media/engine/HostEvents.hpp"
#include "media/engine/HostControl.hpp"
#include "media/ui/PictureAudioCures.hpp"
#include "media/video/Snapshot.hpp"
#include <atomic>
namespace media::engine {
// Surround programmes (more than two channels): Auto sends them to the endpoint as they are when its shared mix
// format has those speakers, and otherwise folds them to stereo (binaural rendering on, S toggles to an ITU
// downmix). Passthrough always sends every channel (Windows converts for a smaller endpoint); Stereo always folds.
enum class SurroundOutput : std::uint8_t { Auto, Passthrough, Stereo };
// What the video track says about its pictures, and what the engine shows (available after Prepare).
struct PictureDescription {
    std::uint32_t coded_width = 0, coded_height = 0;        // The decoder's surface: whole macroblocks or coding blocks.
    std::uint32_t left = 0, top = 0, right = 0, bottom = 0;  // The crop (conformance window), right/bottom exclusive.
    video::SampleAspect bitstream_aspect{0, 0};               // VUI sar_width:sar_height; 0:0 when the stream does not say.
    video::SampleAspect container_aspect{0, 0};               // `pasp`; 0:0 without one.
    video::SampleAspect sample_aspect{};                      // The one used (video::ResolveSampleAspect), in lowest terms,
    video::AspectSource aspect_source = video::AspectSource::Square; // and where it came from.
    video::PictureSize display{};                             // The crop in square pixels: the shape of the viewport.
    video::ColorDescription bitstream_color{}, container_color{}; // VUI video_signal_type; `colr` nclx/nclc.
    video::ColorDescription color{};                          // The one converted with (video::ResolveColor): no code point unspecified.
};
enum class AudioStatus : std::uint8_t {
    None,              // The file has no audio track.
    Ready,             // Decoding and playing.
    UnsupportedLayout, // The track cannot be decoded here (7.1 AAC, HE-AAC, ...): video plays without it.
};
struct AudioRouting {
    AudioStatus status = AudioStatus::None;
    unsigned source_channels = 0, decoded_channels = 0, endpoint_channels = 0, output_channels = 0;
    std::uint32_t source_mask = 0, decoded_mask = 0, endpoint_mask = 0, output_mask = 0;
    bool fold_down = false;  // Surround rendered to stereo by the DSP (binaural or ITU downmix).
    bool converting = false; // The endpoint converts (forced passthrough on a smaller endpoint).
    HRESULT error = S_OK;    // Why an UnsupportedLayout track was dropped.
};
// One track of the media (docs/tracks.md): every trak of the container, and a subtitle sidecar file.
enum class TrackUse : std::uint8_t {
    Selectable,   // Can be chosen now.
    Refused,      // The container reader cannot use it (TrackInfo::refusal): an unknown codec or handler, ...
    Undecodable,  // Its codec is known but not its layout here (7.1 AAC, HE-AAC, AV1 video, ...).
    Incompatible, // Audio at another sample rate or channel count than the endpoint was opened for: chosen at start only.
    Chapters,     // A QuickTime chapter track (tref 'chap'): its samples name the chapters; never offered as subtitles.
};
struct MediaTrack {
    std::uint32_t id = 0;                // Container track id; subtitle sidecar: 0.
    demux::TrackKind kind = demux::TrackKind::Other;
    demux::Codec codec = demux::Codec::Unknown;
    std::uint32_t sample_entry = 0;      // Fourcc of its sample entry ('mp4a', 'ac-3', 'tx3g', ...).
    char language[4]{};                  // ISO 639-2/T ("" when the file does not say).
    char name[32]{};                     // The container's label, when it is not a muxer's handler name.
    unsigned channels = 0, sample_rate = 0, width = 0, height = 0;
    std::uint16_t alternate_group = 0;
    bool enabled = false;                // The file's default among alternatives.
    TrackUse use = TrackUse::Refused;
    std::uint8_t slot = 0;               // Audio: decoder slot; subtitles: channel of the subtitle track.
};
// A track change as the engine applied it (read after Run). Audio changes are spliced in the PCM stream: the new
// track's first kept frame follows the old track's last one at `at` (media time), with a 5 ms fade out and 2.5 ms
// fade in, and the clock is not re-anchored. Before any PCM of the epoch was written (start, a seek landing) the
// change is cold: the new track simply starts at the epoch's target.
struct TrackSwitch {
    std::uint32_t from = 0, to = 0;
    std::int64_t requested = 0, applied = 0; // Now(): the UI's request, the worker's splice.
    std::int64_t at = 0;                     // Media time of the splice (100 ns).
    std::uint64_t frame = 0;                 // PCM frames written since the epoch's origin when it was made.
    std::int64_t queued = -1;                // `at` minus the clock's media time then: old audio still to be heard (-1: no clock).
    bool spliced = false;                    // False: cold (see above).
    // The new track as decoded, and whether it is converted to the output path's format (another rate or layout).
    std::uint32_t rate = 0; unsigned channels = 0; bool converted = false;
};
// A natural-language command (UTF-8, LocalIntentDispatcher's grammar) given `at_ms` after Run starts.
struct ScriptedCommand { unsigned at_ms = 0; const char* text = nullptr; };
// Keys typed into the window `at_ms` after Run starts, as the real messages a keyboard produces: each character is
// a WM_CHAR code unit, except '\r' (the Enter key), '\x1b' (Escape) and '\b' (Backspace), which are WM_KEYDOWN, and
// '\x1d' / '\x1c', which are Ctrl+Right / Ctrl+Left (WM_KEYDOWN with the thread's Control key state down).
struct ScriptedTyping { unsigned at_ms = 0; const wchar_t* keys = nullptr; };
// Voice commands (docs/voice.md). Off unless asked for: nothing opens a microphone by default.
enum class VoiceInput : std::uint8_t {
    Off,
    Microphone, // The default communications capture device.
    Loopback,   // What the default render endpoint plays.
    Script,     // PlaybackOptions::voice_script, fed in real time from Run (tests; no device).
};
struct PlaybackOptions {
    bool visible = true;
    bool activate = true; // False: the window never takes keyboard focus (automated runs).
    // Physical keyboard, mouse, touch and pen input never reaches the window (D3D12Renderer::SetInputIsolation): only
    // scripted input does. Automated runs (the demo's --test-* modes) turn it on.
    bool isolate_input = false;
    // Script harness input posted through the queue (D3D12Renderer::PostTestInput, tagged): Down four times at 1.5 s.
    bool test_isolation = false;
    bool edge_directed = true; // Edge-directed luma reconstruction when the window is larger than the video.
    float sharpness = 0.0f;    // Contrast-adaptive sharpening in linear light, 0 (off) to 1. Opt-in: see docs/picture.md.
    std::uint32_t network_chunk_bytes = 0; // Remote sources: chunk (and largest sample) size; 0 selects 8 MiB.
    // API 15. Remote sources: the host's request headers (HttpSourceOptions::headers; null: none). Any source: set from any
    // thread to stop Prepare (its network probe, its metadata reads); Prepare then fails with stage 1, E_ABORT.
    const wchar_t* http_headers = nullptr;
    const std::atomic<bool>* cancel = nullptr;
    unsigned maximum_seconds = 0;
    const wchar_t* subtitle_path = nullptr; // Local UTF-8 SRT/WebVTT sidecar, loaded in Preparing only. Optional:
                                            // any failure disables subtitles (SubtitleState) and playback continues.
    bool test_subtitles = false;
    bool test_subtitle_overflow = false; // Built-in track with more distinct CJK glyphs than the atlas holds.
    bool test_resize = false; // Script window resizes (landscape, portrait, ultrawide) and a fullscreen round trip.
    SurroundOutput surround = SurroundOutput::Auto;
    // Reconstruction quality (D3D12Renderer::SetQualityMode; docs/picture.md, "Quality governor"): Auto lets the GPU
    // deadline governor choose the tier; the N key cycles the modes. `neural` true fixes it at Neural (as before).
    render::QualityMode quality = render::QualityMode::Auto;
    bool neural = false;
    bool test_neural = false;   // Script N-key presses during playback.
    // Temporal tile reuse on this file (docs/neural.md): fullscreen, Neural fixed, then the same first 5 s three times, with
    // reuse Off, Exact and Tolerant; NeuralReuseRun() has each phase's tiles, GPU time and pictures.
    bool test_neural_reuse = false;
    // Paused inspection (docs/picture.md, "Paused re-conversion"): pause at 1 s, capture the picture on screen, then V, N,
    // C and V again while paused, each followed by a capture once the picture on screen has changed; resume.
    bool test_paused_quality = false;
    // Script the governor (on a long upscaled stream): Auto steps up to Neural; at 4 s extra GPU work (SetTestLoad) puts
    // the Neural and Edge-directed tiers at 90 % of the frame interval and Auto steps down to Fast; at 9 s the load
    // ends and N fixes Neural; at 11 s N three times returns to Auto.
    bool test_quality = false;
    bool test_surround = false; // Script S-key toggles (binaural <-> downmix or passthrough) during playback.
    // Test hooks (0 = off). Each drives the same paths as a real event, never a simulated result.
    unsigned stall_audio_at_ms = 0, stall_audio_ms = 0; // Stop feeding decoded PCM: a genuine endpoint underrun.
    // --test-hiccup (docs/session.md, "The audio lead"): 2 s and 5 s after the clock starts, a 10 ms and a 15 ms gap in the
    // stream (Win32WasapiAudioSink::InjectGap): what a producer a period late leaves; each bridged, the clock never stopping.
    bool test_hiccup = false;
    unsigned pause_at_ms = 0, pause_ms = 0;             // Pause/resume through the UI -> worker command queue.
    bool test_hud = false; // Script core tap, angular drag seek while paused, and keyboard resume through InputHud.
    bool test_seek = false; // Script a forward seek, a paused backward seek and resume; verify each landing frame.
    // Forward seeks inside the group of pictures being decoded carry on decoding instead of restarting from the sync
    // sample (docs/seeking.md, "Forward continuation"). SetSeekContinuation changes it while playing.
    bool seek_continuation = true;
    // Script the same short forward seek twice from the same position (1.5 s -> 2.5 s while playing), with continuation
    // and then without it, with a backward seek between; each landing is recorded like test_seek's.
    bool test_seek_forward = false;
    // Forward seeks to a picture already decoded keep the decoded pictures from the target on instead of decoding again
    // (docs/seeking.md, "Queue seek"). SetQueueSeek changes it while playing.
    bool queue_seek = true;
    // Script queue seeks while playing: 1.0 -> 1.4 s and 1.6 -> 2.1 s, back to 0.9 s, then 1.0 -> 1.4 s again with queue
    // seeks off; each landing is recorded like test_seek's.
    bool test_seek_micro = false;
    // Scrub preview (docs/seeking.md, "Scrub preview"): a backward scrub seek whose exact frame needs more than
    // kPreviewReferences pictures decoded before it shows the target's sync sample alone (flagged as a preview on the
    // HUD); the release, or a drag held still for kScrubSettle, lands exactly. SetScrubPreview changes it while playing.
    bool scrub_preview = true;
    // Script a 120 Hz scrub drag (released like a timeline drag: one exact seek), then a burst of seeks drained
    // together; verify coalescing.
    bool test_scrub = false;
    // Multi-hour files: seek to 1 h and 2 h while playing, back to 45 min while paused, then to 3 h 30 min and play
    // to the end of the stream; each landing is recorded like test_seek's.
    bool test_longform = false;
    // A remote Matroska file (tests/http: media_http_mkv_seek_test, a 10-minute B-picture stream with its Cues at the end): 400.5 s
    // and back to 120.25 s while playing, 530.4 s while paused, 121.7 s, then 595.3 s, playing on to the end. Each landing is recorded
    // like test_seek's (kRemoteSeeks).
    bool test_remote_seek = false;
    bool test_dsp = false; // Exercise the demo's keyboard callback during playback and the seek script.
    bool test_volume = false; // Script volume and mute key presses (Up/Down/M) through the demo's keyboard callback.
    // Commands run through Command() on the UI thread at their times (at most kMaxCommands; the texts must outlive
    // Run). Each outcome is recorded (CommandAt); Run does not end before the last one has been given and landed.
    const ScriptedCommand* commands = nullptr;
    unsigned command_count = 0;
    // Typing through the window procedure (the command bar). Each Enter that submits text is recorded like a scripted
    // command (CommandAt); Run does not end before the last keys have been typed and every seek has landed.
    const ScriptedTyping* typing = nullptr;
    unsigned typing_count = 0;
    render::D3D12Renderer::KeyHandler key_handler = nullptr;
    void* key_context = nullptr;
    // Forensic snapshots (docs/picture.md, "Forensic snapshots", 2026-11-01): where S and "take snapshot" write (UTF-8; the
    // folder must exist; null: platform::DefaultSnapshotFolder).
    const char* snapshot_folder = nullptr;
    // Voice commands: capture -> MicroVad -> VoiceGate -> AcousticIntentRecognizer (the shipped templates) on a voice
    // thread, then Command() on the UI thread. Script: 16 kHz mono samples that must outlive Run.
    VoiceInput voice = VoiceInput::Off;
    const float* voice_script = nullptr;
    std::uint32_t voice_script_frames = 0;
    // The wake word (docs/voice.md, "Wake word"): speech is acted on only after "Avion" (in the same breath, or alone and
    // then a command within voice_wake_window_ms). False: every recognised command runs (the voice path before it).
    bool voice_wake = true;
    std::uint32_t voice_wake_window_ms = 4000;
    const char* wake_word = nullptr; // voice_wake: a name of the host's instead of "Avion" (AvionWakeGate::SetWord).
    // A custom name is made hearable at Prepare (docs/voice.md, "Custom wake words"): the formant synthesiser's renditions,
    // and those of `wake_speech` (a host's text-to-speech) or, without one, of the system's voices (SapiSpeech) when
    // `wake_system_voices`. A name SpellPhonemes cannot read stays text-level (Hear, typed commands).
    const ai::SpeechSource* wake_speech = nullptr;
    bool wake_system_voices = true;
    bool push_to_talk = false;       // voice_wake false: an utterance runs only while SetTalk(true) (the SDK's MEDIA_WAKE_DISABLED).
    // Tracks at start (ISO 639 codes, e.g. "heb", "en"; nullptr: the file's default). Subtitles are shown at start when
    // a sidecar is given, or the file marks a subtitle track as its default, or a language asked for has one.
    const char* audio_language = nullptr;
    const char* subtitle_language = nullptr;
    const char* sidecar_language = nullptr; // The sidecar's language; else a suffix of its name ("film.he.srt").
    // Test hook: measure the committed PCM (a tone and 10 ms notches every 250 ms of media time, as the multi-track
    // fixture carries) to prove what each splice put where (TrackProbe()).
    bool test_tracks = false;
    // The chapter test's script (on the chapter fixtures): `commands` run when what each needs has happened (the previous
    // landing, the position, the live cut for the scene steps) instead of at their times, and Page Up through the window
    // procedure comes between the sixth and the seventh.
    bool test_chapters = false;
    // --test-speed (docs/audio-dsp.md, "Playback speed"), on the multi-track fixture: ] at 1.5 s (1.25x), "speed 2x" at 3 s,
    // "half speed" at 4.5 s, "מהירות רגילה" at 6.5 s, then . three times and , twice (each once the step before has
    // landed), then Escape. SpeedRun() has each speed's phase on the clock, what the committed PCM held at it, and the steps.
    bool test_speed = false;
    // --test-stabilize: the shaken fixture, stabilised from 2.5 s to 6.2 s; SteadyRecord() has the commands' results, the renderer the spots.
    bool test_stabilize = false;
    // --test-zoom (docs/picture.md, "Zoom"): pause at 1 s; capture; "zoom in"; capture; Ctrl+wheel three notches at three
    // quarters across the picture; capture; Left (pan); capture; "אפס זום"; capture; play on. Each capture once the
    // picture on screen has changed. ZoomRun() has every step's zoom, region and image.
    bool test_zoom = false;
    // --test-rotation (docs/picture.md, "Rotation"): at 1 s, playing, the picture as composed (its orientation from the file's
    // tkhd); R; 0.5 s later the picture again; pause; R; the paused picture once converted again; "level horizon right" three
    // times, the picture; "level horizon", then "סובב תמונה" (rotate), the picture; play on. RotationRun() has each picture's
    // 4 x 4 luma grid, its size and the orientation it was shown in.
    bool test_rotation = false;
    // --test-auto-horizon (docs/picture.md, "Auto-leveling the horizon"; the --horizon fixture: +4.5 degrees for 3 s, then
    // -6.0): at 0.5 s quality Fast and the picture as composed; Shift+R; at 2.8 s and at 5.8 s of media time the leveling turn,
    // the horizon's tilt and the picture as composed with its own lines measured (EnableCaptureHorizon). AutoHorizonRun() has
    // them. Fast (bilinear, unsharpened) because the check measures the shown image's lines: through the edge-directed kernel
    // and sharpening at the leveled view's 1.12x zoom they read +0.40 degree where Fast reads 0.00 (the geometry the same: each
    // capture's grid matched the map either way).
    bool test_auto_horizon = false;
    // --test-crop-bars (docs/picture.md, "Black bars"; the --horizon-letterbox fixture: the same pictures with 40 rows of black
    // encoded at the top and the bottom): the auto-horizon script, and after the picture at 2.8 s "recadrer les bandes noires"
    // (crop black bars); AutoHorizonRun() also has the bars found, the view's region and where each shown image has picture.
    bool test_crop_bars = false;
    // --test-zoom-live (docs/picture.md, "Live zoom"): while playing, "zoom in" at 1.5 s, the wheel at 3 s, a pan at 4.5 s and
    // "reset zoom" at 6 s; D3D12Renderer::LiveZoomAt has when each was on screen, through the queue and natively.
    bool test_zoom_live = false;
    // --test-tracking (docs/picture.md, "Target tracking"): on the tracking fixture, T at 1.0 s of media (the square is in the
    // middle of the view); TrackRun() has every picture shown after it: its framing and the tracker's state.
    bool test_tracking = false;
    // --test-next-target (docs/picture.md, "Several objects at once", 2026-10-31): on the two-targets fixture, T at 1.0 s of media,
    // Tab on the very next picture shown (2026-11-01: the table is warm), Tab at 2.0 s, "cible suivante" at 3.5 s, S at 4.2 s;
    // TargetRecord() has, 0.6 s of media after each, the view, the audio focus, the tracker and the object table, what the first
    // Tab did at once and the snapshot S wrote.
    bool test_next_target = false;
    // --test-vibrometry (docs/picture.md, "Remote vibrometry", 2026-11-02): on the pulse fixture, T at 1.0 s; at 6.0 s the
    // HUD's readout is taken, tracking stops and "micro motion on" starts the warm grids' measure; at 11.5 s the readout again.
    // VibrometryRecord() has both.
    bool test_vibrometry = false;
    // --test-saccade (docs/audio-dsp.md, "Sound localisation and the audio saccade"): on the saccade fixture, "look at sound" at
    // 1.0 s; a burst on the right at 2 s and on the left at 4 s. SaccadeRecord() has the view after each.
    bool test_saccade = false;
    // --test-ab-loop (docs/seeking.md, "A–B loop"): L at 1.0 s and at 2.0 s (the loop [1, 2] turns three times), L (off), then
    // "loop this sentence" on the cue on screen (three turns), "בטל לופ"; LoopRun() has every turn.
    bool test_ab_loop = false;
    // --test-telemetry (docs/session.md, "Telemetry readout"): I (the panel) at 1 s, extra GPU load from 2 s until the governor
    // steps down, then "why did quality change?", "הצג נתונים", "status report", I; TelemetryRun() has every readout.
    bool test_telemetry = false;
    // --test-bookmarks (docs/session.md, "Bookmarks"): B, "add bookmark", "סמן הערה" while playing; then "go to bookmark 1",
    // "next bookmark", "לך לסימנייה 3", "previous bookmark", each once the last has landed; Ctrl+B (pin), pause, "go to
    // bookmark 3", V twice (Pinned | Live) and the three captures. BookmarkRun() has the record.
    bool test_bookmarks = false;
    // --test-music (docs/audio-dsp.md, "Music mode"), on a file with AAC alone: from 1 s to 2 s of media, the sound's
    // composite rate; at 2 s the loudest band shown, then "next chapter"; 0.6 s after its landing the loudest band again,
    // then a 0.5 s pause (no picture may come meanwhile); "speed 1.5x" at 6.5 s; the loudest band at 8 s; then on to the
    // end. MusicRun() has the record.
    bool test_music = false;
    // ---- Sessions (docs/session.md) ----
    // Smart resume: the ledger of where files were left (the caller's; nullptr: none). A local file found in it starts with
    // the tracks chosen last time, and at the position it was left when that is past the first 15 s and more than 60 s
    // before the end (a status cue says so; Home starts over). When Run ends, the position and tracks are written back
    // (UI thread; the caller saves the ledger).
    session::ResumeLedger* resume = nullptr;
    // The folder's playable files before and after this one (session::Playlist). "next video", "הסרטון הקודם", Ctrl+Right
    // and Ctrl+Left need one that way: they end Run with RequestedFile() set, and the caller opens it. With either set,
    // the start's status cue names the file and its place ("2 of 5 · Episode 2.mp4").
    unsigned files_before = 0, files_after = 0;
    // The previous file's window (KeepWindow, HandedWindow): this file plays in it instead of a new window.
    render::WindowHandoff window{};
    // ---- Modules (docs/sdk.md, "Modules") ----
    // EngineModuleFlags: a module left out takes no arena memory (its region is not made: the arena's capacity is
    // ArenaBudget(modules)) and starts no thread. Voice needs `voice` too; the others work as before when enabled.
    std::uint32_t modules = kAllModules;
    FrameCallback frame_callback = nullptr; // kModuleTelemetry: each picture's signature as it is put on screen.
    void* frame_context = nullptr;
    // Low-power pacing (docs/session.md, "Low-power pacing"): between pictures the presentation thread waits on a
    // high-resolution waitable timer, window messages and the worker's next picture instead of spinning, and spins only the
    // last kPaceSpin before a picture's deadline. False: it spins every pass (the engine before 2026-10-22).
    bool low_power_pacing = true;
    // Paused and settled (2026-11-03; docs/session.md, "Low-power pacing"): no picture due, the controls still, the GPU idle.
    // The presentation thread then sleeps up to 250 ms between advances (not 8.3 ms) and the worker up to 250 ms (not 10 ms):
    // input, commands, pictures, a host's stop and a voice command wake them at once, a scripted command, typing or pause
    // at its time. False: the 8.3 / 10 ms of before, for test scripts that act during a pause by polling.
    bool settled_sleep = true;
    // The spin before a picture's deadline (microseconds; its due-at-once floor is 1.5x it). The high-resolution timer wakes
    // 0.3-0.4 ms late on average, so the shorter the spin the more Presents come after their deadline: 1,200 (before
    // 2026-11-03) put 0-1 of ~600 a run past it at 8.3-10.6 % of a core at 60 fps; 450 (the default) 2.5-6.9 %, with 25-30 % of
    // Presents past it by 0.07-0.13 ms on average (1.25 ms at most) and no picture dropped. The demo's Pacing line counts
    // them (docs/session.md, "The pre-deadline spin"). Up to 2,000; 1,200 for frame-exact Presents.
    std::uint32_t pace_spin_us = 450;
    // Perception's warm grids while nothing asks for it (D3D12Renderer::SetWarmPerception): one grid every 100 ms of media, so
    // that T and Tab start from objects already known. False: playback alone (the demo's --no-perception; docs/benchmarks.md).
    bool warm_perception = true;
    // The window thread (docs/session.md, "The window thread"): the window and its message dispatch on a thread of their own,
    // input handed to the presentation thread through a lock-free ring (D3D12Renderer::SetMessageThread). False: the window
    // is the presentation thread's, as before 2026-10-28 (a stalled dispatch then stalls pictures; demo --window-thread-off).
    bool message_thread = true;
    // --test-window-stall (docs/session.md, "The window thread"): while playing, the window's procedure blocks for 120 ms at
    // 1, 2 and 3 s (D3D12Renderer::StallWindow), and a test click (WM_MOUSEMOVE, tagged) is posted every 250 ms: no picture
    // may be dropped, and InputStatistics() says how long input waited between its window thread and the presentation thread.
    bool test_window_stall = false;
    // Embedding (docs/sdk.md, "Embedding: API 12"): the host's window to play inside, as a borderless child filling its client
    // area and following its size (D3D12Renderer::SetParentWindow). Null: the engine's own window.
    HWND parent_window = nullptr;
    // Texture output (docs/sdk.md, "Texture output: API 13"): the pictures drawn into three shared textures of this size for the
    // host's renderer instead of a window on the screen (D3D12Renderer::SetTextureOutput); 0: off.
    unsigned texture_width = 0, texture_height = 0;
    render::SharedOutput::Format texture_format = render::SharedOutput::Format::Rgba8; // API 14: 8-bit, PQ10 or scRGB FP16.
};
// A warm session (docs/session.md, "Warm switch"): what one file's coordinator leaves to the next, so that changing files
// creates no window, device, swap chain, pipelines, decoder, audio endpoint or arena again when the next file can use
// them. Owned by the caller (one per window), handed to each file's coordinator (PlaybackCoordinator(WarmSession*)); the
// first file creates what is in it, later files take it as it is or replace what does not fit them:
// - the file arena (ArenaBudget(modules): 239 MiB with every module) is committed and touched once, then rewound
//   (LinearMemoryArena::Reset) for every file that asks for the same modules;
// - the window, device, queue, fence, swap chain and the passes that do not depend on the video (render::GpuSession);
// - the video decoder when it is Compatible (codec, size, bit depth, surfaces, bitstream capacity), restarted at epoch 1;
//   otherwise it is closed and made again in its own arena;
// - the audio endpoint when the next file opens it with the same options (format, layout, conversion), stopped and reset
//   at the end of each file; otherwise it is closed and opened again.
// Close (also the destructor) releases everything after the last coordinator has closed. UI thread.
struct WarmSession {
    WarmSession() noexcept;
    WarmSession(const WarmSession&) = delete;
    WarmSession& operator=(const WarmSession&) = delete;
    ~WarmSession() noexcept { Close(); }
    void Close() noexcept;
    struct Reuse { unsigned files = 0, arenas_kept = 0, gpu_kept = 0, slots_kept = 0, decoders_kept = 0, decoders_made = 0, endpoints_kept = 0, endpoints_made = 0; };
    const Reuse& Stats() const noexcept { return reuse; }
    render::GpuSession gpu;
    core::LinearMemoryArena arena, decoder_arena, sink_arena;
    platform::windows::D3D12VideoDecoder decoder;
    platform::windows::Win32WasapiAudioSink sink;
    platform::windows::WasapiOptions sink_options{};
    bool sink_ready = false;
    Reuse reuse{};
private:
    CO_MTA_USAGE_COOKIE mta_ = nullptr; // The endpoint made on one worker thread is used on the next: the MTA stays.
};
// UI thread -> media worker (SPSC). Cache-line sized like every transported record.
struct alignas(64) ControlCommand {
    enum class Kind : std::uint32_t { Pause, Resume, Seek, SelectAudio, Speed, LoopAnchor } kind;
    std::uint32_t epoch;       // Seek: the new playback epoch (strictly increasing). SelectAudio: the catalogue index.
                               // Speed: the playback speed in thousandths (250 to 2000).
    std::int64_t target_100ns; // Seek: requested presentation time. LoopAnchor: the loop's A (INT64_MIN: no loop).
                               // A pinned loop turn (reserved[1] bit 0) seeks to the end of the pinned pictures; its A is
                               // reserved[2] | reserved[3] << 32.
    std::uint32_t reserved[12]; // Explicit padding to one cache line.
};
static_assert(sizeof(ControlCommand) == 64);
class PlaybackCoordinator final {
public:
    // `warm`: a warm session to take the window, device, decoder, endpoint and arena from and to leave them to (nullptr: this
    // coordinator makes and releases its own, as a single file does).
    explicit PlaybackCoordinator(WarmSession* warm = nullptr) noexcept;
    PlaybackCoordinator(const PlaybackCoordinator&) = delete;
    PlaybackCoordinator& operator=(const PlaybackCoordinator&) = delete;
    ~PlaybackCoordinator() noexcept;
    bool Prepare(const wchar_t* path, PlaybackOptions = {}) noexcept;
    bool Run() noexcept;
    void Close() noexcept;
    unsigned Presented() const noexcept { return renderer_.Presented(); }
    unsigned Dropped() const noexcept { return renderer_.Dropped(); }
    unsigned OutOfOrder() const noexcept { return renderer_.OutOfOrder(); }
    unsigned ReorderDepth() const noexcept { return renderer_.ReorderDepth(); }
    unsigned ToneMapped() const noexcept { return renderer_.ToneMapped(); } // PQ/HLG frames tone-mapped on the GPU. // sps_max_num_reorder_pics (0 for H.264 I/P).
    unsigned HudPresents() const noexcept { return renderer_.HudPresents(); }
    unsigned PausedHudPresents() const noexcept { return renderer_.PausedHudPresents(); }
    unsigned RejectedSeeks() const noexcept { return renderer_.RejectedSeeks(); }
    float HudScrubOffset() const noexcept { return renderer_.HudState().scrub_offset; } // Winding lane length (fraction).
    std::uint32_t LandedEpoch() const noexcept { return renderer_.LastSeek().epoch; } // Newest seek whose frame was shown.
    unsigned Hidden() const noexcept { return hidden_; }  // Decoded reference-only pictures (read after Run).
    unsigned Discarded() const noexcept { return discarded_.load(); } // RASL pictures never decoded.
    // Pictures neither shown nor referenced (non-reference pictures before a seek target or an edit start): not decoded.
    unsigned Skipped() const noexcept { return skipped_.load(); }
    unsigned Discontinuities() const noexcept { return discontinuities_.load(); } // Clock re-anchors after underrun/pause.
    // The audio lead (docs/session.md, "The audio lead", 2026-10-30): short underruns bridged without stopping the clock, the
    // times the worker read past a video packet waiting for a surface to keep the endpoint's PCM, and the least PCM queued
    // for the endpoint while playing (from 1 s in; -1: none measured).
    unsigned BridgedGaps() const noexcept { return bridged_gaps_.load(std::memory_order_relaxed); }
    unsigned ReadAheads() const noexcept { return read_aheads_.load(std::memory_order_relaxed); }
    std::int64_t LeastAudioLead() const noexcept { return least_lead_.load(std::memory_order_relaxed); } // 100 ns.
    std::uint64_t SilenceFrames() const noexcept { return silence_frames_.load(); } // Endpoint-inserted underrun silence.
    // UI thread: queue a pause/resume for the media worker. False when the queue is full.
    bool RequestPause(bool pause) noexcept;
    bool Paused() const noexcept { return paused_; }
    // UI thread: queue an epoch-bumping seek. False for fragmented input or a full queue. Stale frames are
    // flushed from presentation immediately; the media worker repositions demux, codec, audio and clock.
    // `scrub`: continuous dragging. At most one scrub seek is in flight; newer targets replace a single pending
    // slot and the newest is sent once the in-flight seek's first frame is on screen, so every decode that runs
    // ends in a visible frame and superseded targets never start one. A non-scrub seek is sent immediately.
    bool RequestSeek(std::int64_t target_100ns, bool scrub = false) noexcept;
    // UI thread: seek to the start of the `match_index`-th cue (in time order) whose displayed text contains `query`,
    // ignoring letter case, Hebrew points and Arabic harakat (subtitle::SearchIndex, MatchMode::Folded). When the quote
    // is in no cue, the `match_index`-th cue that means it, best first (ai::ConceptIndex: "danger" finds "careful",
    // "זהירות", "מלכודת"). The lookup reads fixed tables and the stack only. The landing frame is the one on screen at the
    // cue's start, shown with that cue. False: no subtitles, no such match, or the seek was refused.
    bool SeekToSubtitle(const char* query, unsigned match_index = 0) noexcept;
    bool SeekToSubtitle(std::string_view query, unsigned match_index = 0) noexcept;
    // lookup: 100 ns. by_concept: found by concept search, with its confidence (ai::ConceptMatch).
    struct SubtitleSeek { bool found = false, sent = false, by_concept = false; unsigned cue = 0; std::int64_t start = 0, lookup = 0; float confidence = 0; };
    // The cues a quote finds now: those containing it, else those meaning it (as SeekToSubtitle chooses).
    unsigned SubtitleMatches(std::string_view query) const noexcept;
    const SubtitleSeek& LastSubtitleSeek() const noexcept { return subtitle_seek_; }
    const subtitle::SearchIndex& Search() const noexcept { return search_; }
    // ---- Playback speed and stepping (docs/audio-dsp.md, "Playback speed") ----
    // UI thread. 0.25x to 2x, pitch preserved (audio::TimeStretcher), heard from the PCM decoded next and followed exactly
    // by the clock (video at the same speed). False: out of range, or a full control queue. A status cue says the speed.
    static constexpr float kSpeeds[]{0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f}; // ] and [ step through these.
    bool SetSpeed(float speed) noexcept;
    bool StepSpeed(int direction) noexcept; // The next step of kSpeeds up (> 0) or down from the speed now.
    float Speed() const noexcept { return speed_; }
    // UI thread: one picture forward (direction > 0) or back, pausing first: a seek to the picture after the one on screen
    // (its end) or before it (its start less 100 ns), landed exactly (docs/seeking.md). Steps asked for while one lands
    // follow it, one at a time. False: not seekable, or nothing shown yet.
    bool StepFrame(int direction) noexcept;
    unsigned FrameSteps() const noexcept { return frame_steps_; } // Steps landed.
    // ---- Sessions (docs/session.md) ----
    enum class FileRequest : std::uint8_t { None, Next, Previous };
    // UI thread: end Run so that the caller opens the next (direction > 0) or previous file of the folder. NotFound, with a
    // warning cue, when there is none that way (PlaybackOptions::files_before / files_after).
    ai::IntentStatus RequestFile(int direction) noexcept;
    FileRequest RequestedFile() const noexcept { return file_request_; }
    // Before Close: the window stays open, where it is and as large, for the next file (PlaybackOptions::window).
    void KeepWindow() noexcept { renderer_.KeepWindow(); }
    const render::WindowHandoff& HandedWindow() const noexcept { return renderer_.HandedWindow(); }
    bool AdoptedWindow() const noexcept { return renderer_.Adopted(); }
    // What resume did (read after Run): the entry found, the start-up seek and its landing, and what was written back.
    struct ResumeRecord {
        bool known = false;    // The ledger had this file.
        session::ResumeEntry entry{};
        bool resumed = false;  // Started at the entry's position.
        std::int64_t target = 0; std::uint32_t epoch = 0;
        bool landed = false; std::int64_t pts = 0, duration = 0, latency = 0;
        bool restored_audio = false, restored_subtitles = false; // The entry's tracks were the ones chosen at start.
        bool recorded = false; session::ResumeEntry written{}; // Written back when Run ended.
        char cue[ui::StatusCue::kMaxBytes]{};                 // The start's status cue ("" when none).
    };
    const ResumeRecord& Resume() const noexcept { return resume_; }
    const ai::ConceptIndex& Concepts() const noexcept { return concepts_; }
    std::int64_t SearchPrepareTime() const noexcept { return search_prepare_; } // 100 ns spent building the index.
    // UI thread: natural-language commands (docs/intents.md). Bounds() is what a command is validated against now;
    // Command() parses, validates and, when the intent is Ok, executes it; Execute() applies an already typed intent.
    static constexpr unsigned kMaxCommands = 16;
    ai::MediaBounds Bounds() const noexcept;
    ai::Intent Command(std::string_view utf8) noexcept;
    ai::IntentStatus Execute(const ai::Intent&) noexcept;
    // What the command bar does on Enter (docs/command-palette.md): the text is executed as a command when the
    // grammar reads one, and otherwise looked for as a quote in the subtitles (LocalIntentDispatcher::Interpret).
    ai::Intent Submit(std::string_view utf8) noexcept;
    const ai::LocalIntentDispatcher::Statistics& CommandStats() const noexcept { return dispatcher_.Stats(); }
    // What follow-ups refer to (docs/intents.md, "Follow-ups"): the last quote and match, where playback was before the
    // last jump (every user jump: commands, keys, the dock's timeline from where a drag began), the last repeatable command.
    const ai::ConversationState& Conversation() const noexcept { return conversation_; }
    struct NeuralReusePhase {
        std::uint32_t reused = 0, evaluated = 0; std::int64_t gpu_sum = 0; unsigned gpu_count = 0; unsigned presented = 0, dropped = 0;
        unsigned dropped_by[4]{}; // Of `dropped`, those within 0.5, 1, 2 and 3 s of the phase's landing (the queue refilling).
    };
    struct NeuralReuseRun { NeuralReusePhase phase[3]{}; bool done = false; };
    const NeuralReuseRun& NeuralReuse() const noexcept { return neural_reuse_run_; }
    unsigned PausedRefreshes() const noexcept { return paused_refreshes_; } // Refresh landings asked by the renderer while paused.
    // Scripted commands (read after Run): the intent, the engine state right after it, and for a seek its landing.
    struct CommandResult {
        ai::Intent intent{};
        std::int64_t resolve = 0;       // 100 ns from the text to the engine action (parse, validate, look up, enqueue).
        float volume = 0; bool muted = false, paused = false;
        std::uint32_t epoch = 0;        // Seeks: the epoch it started.
        bool landed = false;
        std::int64_t pts = 0, duration = 0, latency = 0;
        render::D3D12Renderer::SubtitleFrame subtitles{};
        std::uint32_t audio_track = 0, subtitle_track = 0; // AudioTrackId(), SubtitleTrackId() right after it.
        bool by_concept = false;                 // FindSubtitle: found by concept search (LastSubtitleSeek).
        char status[ui::StatusCue::kMaxBytes]{}; // The status cue's text right after it.
    };
    unsigned CommandResults() const noexcept { return command_results_; }
    // Voice commands heard (read after Run): the recognised text, and what Command() made of it.
    struct VoiceResult {
        char text[128]{};
        ai::Intent intent{};             // `query` cleared: it viewed the text.
        float distance = 0;              // Of the matched template (AcousticIntentRecognizer).
        std::int64_t heard = 0;          // 100 ns after Run started, when the UI thread received it.
        float volume = 0; bool muted = false, paused = false; // Engine state right after the command.
        ai::WakeDecision wake = ai::WakeDecision::Command; // Listening: the name alone (no command; `intent` empty).
    };
    static constexpr unsigned kMaxVoiceResults = 16;
    unsigned VoiceResults() const noexcept { return voice_results_; }
    const VoiceResult& VoiceAt(unsigned i) const noexcept { return voice_[i < kMaxVoiceResults ? i : kMaxVoiceResults - 1]; }
    // Voice path state: whether it runs, its capture error, and the detector's, gate's and recogniser's counts.
    struct VoiceState {
        bool active = false; HRESULT error = S_OK; std::uint32_t sample_rate = 0;
        audio::VadStatistics detector{}; ai::VoiceGateStatistics gate{}; ai::AcousticIntentRecognizer::Statistics recogniser{};
        ai::WakeStatistics wake{};
        std::uint64_t captured = 0, dropped = 0; bool script_done = false;
    };
    VoiceState Voice() const noexcept;
    const CommandResult& CommandAt(unsigned i) const noexcept { return commands_[i < kMaxCommands ? i : kMaxCommands - 1]; }
    unsigned Seeks() const noexcept { return seeks_; }             // Seek commands sent to the worker.
    unsigned AppliedSeeks() const noexcept { return applied_seeks_.load(); }       // Repositionings performed.
    unsigned SupersededSeeks() const noexcept { return superseded_seeks_.load(); } // Drained but not applied.
    unsigned CoalescedScrubs() const noexcept { return coalesced_scrubs_; }         // Scrub targets never sent.
    struct ScrubResult {
        unsigned requests = 0, landings = 0;           // Scrub requests issued; distinct seek frames shown while dragging.
        unsigned previews = 0;                         // Of `landings`: keyframe previews (the others are exact).
        // The two halves of the drag (forward 0.5 -> 2.4 s, then backward to 1.1 s): frames shown, longest gap.
        unsigned forward_landings = 0, backward_landings = 0;
        std::int64_t forward_gap = 0, backward_gap = 0;
        bool released = false;                          // The release sent an exact seek (else the last scrub seek was exact).
        // Each frame shown while dragging (the first kMaxTrace): when (100 ns from the drag's start), the request-to-screen
        // latency of its seek, its PTS, the target it was sent for, and whether it was a preview.
        static constexpr unsigned kMaxTrace = 64;
        struct Landing { std::int64_t at = 0, latency = 0, pts = 0, target = 0; bool preview = false; } trace[kMaxTrace]{};
        std::int64_t target = 0, pts = 0, duration = 0; // Final drag target and its landing frame.
        std::int64_t final_latency = 0, max_gap = 0;   // Release (last request) -> final frame; longest wait between frames.
        std::int64_t burst_target = 0, burst_pts = 0, burst_duration = 0, burst_audio = 0;
        unsigned burst_superseded = 0; bool done = false;
        render::D3D12Renderer::SubtitleFrame subtitles{}, burst_subtitles{}; // Cues on the two landing frames.
    };
    const ScrubResult& Scrub() const noexcept { return scrub_; }
    // Per-seek landing record of the scripted test (read after Run).
    // latency: request to first target frame on screen; resumed: Now() when the clock re-anchored afterwards.
    struct SeekResult {
        std::int64_t target = 0, requested = 0, pts = 0, duration = 0, latency = 0, resumed = 0, audio_start = 0; bool landed = false, paused = false;
        render::D3D12Renderer::SubtitleFrame subtitles{}; // Cues composited into the landing frame's image.
        std::uint32_t epoch = 0;                           // The epoch the seek started.
    };
    unsigned SeekResults() const noexcept { return seek_results_; }
    static constexpr unsigned kMaxSeekResults = 6;
    const SeekResult& SeekAt(unsigned i) const noexcept { return results_[i < kMaxSeekResults ? i : kMaxSeekResults - 1]; }
    // What each applied seek cost the decoder (worker-written; read after Run). `resumed`: for a forward continuation,
    // the pictures from the sync sample on that were not read and decoded again; `references`: pictures decoded but not
    // shown (they end before the target); `skipped`: pictures before the target neither shown nor referenced, never
    // decoded. `adopted`: a queue seek (the target was among pictures already decoded: nothing was decoded again);
    // `kept`: its landing picture had already been converted when it was asked for.
    // `preview`: a scrub preview (only the target's sync sample was decoded and shown).
    struct SeekWork { std::uint32_t epoch = 0; bool continued = false; unsigned resumed = 0, references = 0, skipped = 0; bool adopted = false, kept = false, preview = false; };
    static constexpr unsigned kMaxSeekWork = 128;
    unsigned SeekWorks() const noexcept { return work_count_.load(std::memory_order_acquire); }
    const SeekWork* WorkFor(std::uint32_t epoch) const noexcept; // Null: no applied seek started it (or past the record).
    unsigned ContinuedSeeks() const noexcept { return continued_seeks_.load(); } // Applied seeks that were continuations.
    unsigned AdoptedSeeks() const noexcept { return adopted_seeks_.load(); }     // Applied seeks that were queue seeks.
    void SetQueueSeek(bool enabled) noexcept { queue_seek_.store(enabled, std::memory_order_relaxed); } // Any thread.
    void SetSeekContinuation(bool enabled) noexcept { continuation_.store(enabled, std::memory_order_relaxed); } // Any thread.
    // Scrub preview (docs/seeking.md): reference pictures a scrub seek may decode before its exact frame; beyond that it
    // shows the sync sample alone. A drag held still this long on a preview lands exactly (100 ns).
    static constexpr unsigned kPreviewReferences = 2;
    static constexpr std::int64_t kScrubSettle = 2500000;
    void SetScrubPreview(bool enabled) noexcept { scrub_preview_.store(enabled, std::memory_order_relaxed); } // Any thread.
    unsigned PreviewSeeks() const noexcept { return preview_seeks_.load(); } // Applied seeks shown as keyframe previews.
    bool ShowingPreview() const noexcept { return showing_preview_; }       // UI: the picture on screen is a preview.
    unsigned SettledScrubs() const noexcept { return settled_scrubs_; }      // Exact seeks sent for a drag held still.
    unsigned StaleFrames() const noexcept { return renderer_.Stale(); }
    // Sample index: pool bytes held by the demuxer's compact tables, and the samples they describe.
    std::uint64_t IndexBytes() const noexcept { return parser_.IndexBytes(); }
    std::uint64_t IndexedSamples() const noexcept { return std::uint64_t{video_.sample_count} + audio_.sample_count; }
    bool Remote() const noexcept { return remote_; }
    platform::windows::HttpSourceStatistics Network() const noexcept { return http_.Statistics(); } // Remote sources only.
    unsigned AudioFrames() const noexcept { return audio_frames_.load(); }
    unsigned AudioRate() const noexcept { return audio_.sample_rate; } // Hz; 0 without a decodable audio track.
    unsigned SubtitlePresents() const noexcept { return renderer_.SubtitlePresents(); }
    // The subtitle track shown from: the one made at open, or (API 17) the last one attached.
    const subtitle::SubtitleTrack& Captions() const noexcept { return *live_track_.load(std::memory_order_acquire); }
    // Subtitles after open (API 17, media_engine_attach_subtitles): a .srt/.vtt (UTF-8 path, at most kMaxAttachBytes) parsed on
    // the caller's thread into the free one of two states reserved at open, then handed to the window's thread, which rebinds the
    // subtitle pipeline to it (waited for, at most 3 s). It becomes the sidecar and is shown; the file's own tracks stay.
    enum class AttachResult : std::uint8_t { Ok, Unsupported, Busy, Unreadable, Timeout };
    static constexpr std::uint32_t kMaxAttachBytes = 4u << 20;
    [[nodiscard]] AttachResult AttachSubtitles(const char* utf8_path) noexcept;
    // Audio decoding and output decisions; complete once Prepare has returned.
    const AudioRouting& Routing() const noexcept { return routing_; }
    // ---- Tracks (docs/tracks.md) ----
    // The catalogue: every container track (refused ones included) and a subtitle sidecar. Complete after Prepare.
    static constexpr unsigned kMaxTracks = 17, kMaxAudioTracks = 8, kMaxSwitches = 16;
    static constexpr std::uint32_t kNoTrack = UINT32_MAX;
    unsigned Tracks() const noexcept { return track_count_; }
    const MediaTrack& TrackAt(unsigned i) const noexcept { return tracks_[i < kMaxTracks ? i : 0]; }
    const MediaTrack* FindTrack(std::uint32_t id, demux::TrackKind kind) const noexcept;
    // UI thread. Audio: the Selectable audio track `id` replaces the one playing, spliced into the PCM stream by the
    // worker (TrackSwitch); video, clock and subtitles are untouched. Subtitles: track `id` (the sidecar is 0), or
    // kNoTrack for none, shown from the next composite and searched from now on. False: no such selectable track, or a
    // full control queue; asking for the current track is true and does nothing. Each change shows a status cue.
    bool SelectAudioTrack(std::uint32_t id) noexcept;
    bool SelectSubtitleTrack(std::uint32_t id) noexcept;
    std::uint32_t AudioTrackId() const noexcept { return audio_choice_; }       // UI: the track asked for last.
    std::uint32_t SubtitleTrackId() const noexcept { return subtitle_choice_; } // kNoTrack: none shown.
    // API 16, any thread: the choices as Run last published them (0 / kNoTrack before the first pass).
    std::uint32_t PublishedAudioTrack() const noexcept { return published_audio_track_.load(std::memory_order_relaxed); }
    std::uint32_t PublishedSubtitleTrack() const noexcept { return published_subtitle_track_.load(std::memory_order_relaxed); }
    unsigned TrackSwitches() const noexcept { return switch_count_.load(std::memory_order_acquire); } // Applied by the worker.
    const TrackSwitch& SwitchAt(unsigned i) const noexcept { return switches_[i < kMaxSwitches ? i : kMaxSwitches - 1]; }
    std::int64_t SubtitleImportTime() const noexcept { return subtitle_import_; } // 100 ns reading embedded subtitle samples.
    // --test-tracks (options.test_tracks): what the committed PCM showed. Notches are the fixture's time marks; their
    // distance from the media time the clock gives them measures alignment across every splice.
    struct TrackProbe {
        unsigned notches = 0;           // Notches found away from splices.
        std::int64_t worst_error = 0;   // Largest |notch time - nearest 250 ms mark| (100 ns).
        unsigned gaps = 0;              // 1 ms windows of silence that were neither a notch nor a splice's fade.
        struct Splice { float before_hz = 0, after_hz = 0, dip = 0; std::uint64_t frame = 0; } splices[kMaxSwitches]{};
        unsigned splice_count = 0;      // `dip`: quietest 1 ms around the splice, relative to the tone.
    };
    const TrackProbe& Probe() const noexcept { return probe_; }
    // --test-speed's record (read after Run). Phases: the clock's speed from one change to the next, with the clock's media
    // time and Now() at both ends. Audio: per speed, the committed PCM's tone (zero crossings of 10 ms windows that held
    // it) and its 250 ms marks, each placed in media time through the speed segments and compared with the grid the first
    // mark sets. Steps: the picture on screen after each frame step landed.
    struct SpeedRecord {
        struct Phase { float speed = 0; std::int64_t from_now = 0, from_media = 0, to_now = 0, to_media = 0; };
        struct Audio { float speed = 0; unsigned windows = 0, marks = 0, gaps = 0; double hz_sum = 0; std::int64_t worst = 0; };
        Phase phases[8]{}; unsigned phase_count = 0;
        Audio audio[8]{}; unsigned audio_count = 0;
        std::int64_t steps[8]{}, start_pts = 0; unsigned step_count = 0;
        bool done = false;
    };
    const SpeedRecord& SpeedRun() const noexcept { return speed_run_; }
    struct SteadyRun { std::int64_t on_at = 0, off_at = 0; std::uint32_t pictures = 0, measured = 0; ai::IntentStatus status_on = ai::IntentStatus::Empty, status_again = ai::IntentStatus::Empty; bool off_shown = false, done = false; };
    const SteadyRun& SteadyRecord() const noexcept { return steady_run_; }
    struct ZoomRecord {
        struct Step { float zoom = 1, x = 0.5f, y = 0.5f; render::D3D12Renderer::Region region{}; std::uint64_t left = 0, right = 0; std::int64_t latency = 0; };
        Step steps[5]{}; unsigned count = 0; bool done = false;
    };
    const ZoomRecord& ZoomRun() const noexcept { return zoom_run_; }
    struct RotationRecord {
        struct Step { video::Orientation orientation{}; unsigned width = 0, height = 0; float grid[16]{}; bool paused = false; };
        Step steps[5]{}; unsigned count = 0; bool done = false;
    };
    const RotationRecord& RotationRun() const noexcept { return rotation_run_; }
    struct AutoHorizonRecord {
        struct Step { std::int64_t position = 0; float level = 0, tilt = 0; bool locked = false, on = false; video::Orientation orientation{};
                      unsigned width = 0, height = 0; float grid[16]{}; video::HorizonLevel::Measurement shown{};
                      bool cropped = false, bars = false; video::Matte::Rect active{}, extents{}; bool extents_found = false; render::SampleRect region{}; };
        Step steps[3]{}; unsigned count = 0, measured = 0, readbacks = 0; bool done = false;
    };
    const AutoHorizonRecord& AutoHorizonRun() const noexcept { return auto_horizon_run_; }
    unsigned WindowStalls() const noexcept { return stall_script_; } // --test-window-stall: stalls posted.
    // Music mode (docs/audio-dsp.md, "Music mode"): the file has no picture track, its AAC plays alone and the window shows
    // its sound. The worker analyses the PCM it hands to the endpoint (audio::SpectrumAnalyser, per 1024 frames) into a ring
    // of pictures stamped with their media time; the UI thread hands the renderer the latest one the clock has reached.
    bool MusicMode() const noexcept { return music_; }
    struct MusicRecord {
        unsigned published = 0, dropped = 0;        // Made by the worker / not queued (the ring was full). After Run.
        unsigned shown = 0, skipped = 0, stale = 0; // Handed to the renderer / passed over (several due at once) / of a left epoch.
        std::int64_t lag_min = INT64_MAX, lag_max = 0, lag_sum = 0; // On screen (media, as the clock expects it) - picture media.
        // --test-music: composites a second while playing, the loudest band shown at its three marks and the band each
        // tone falls in, the chapter landing (media, latency), and what came while paused.
        double rate = 0; std::int64_t rate_from = 0; unsigned rate_presents = 0;
        unsigned bands[3]{}, expected[3]{};
        std::int64_t landing = 0, landing_latency = 0;
        unsigned paused_pictures = 0, paused_presents = 0;
        bool done = false;
    };
    const MusicRecord& MusicRun() const noexcept { return music_run_; }
    unsigned SpatialToggles() const noexcept { return spatial_toggles_; }
    void SetClarityLevel(unsigned level) noexcept { dsp_controls_.clarity.store(level > 3 ? 3 : level, std::memory_order_relaxed); }
    void SetSpatialAudio(bool enabled) noexcept { dsp_controls_.spatial.store(enabled, std::memory_order_relaxed); }
    unsigned ClarityLevel() const noexcept { return dsp_controls_.clarity.load(std::memory_order_relaxed); }
    bool SpatialAudio() const noexcept { return dsp_controls_.spatial.load(std::memory_order_relaxed); }
    // Worker-owned statistics: read only after Run has stopped the worker, or after Close joins it.
    const audio::DspStatistics& DspStats() const noexcept { return dsp_.Statistics(); }
    unsigned DspResets() const noexcept { return dsp_resets_; }
    // Output volume (0 to 1, a 60 dB perceptual taper: 5 % is 3 dB; API 16: to 2, a boost through a soft limiter) and mute:
    // any thread, lock-free, heard within the endpoint's buffer and ramped over 20 ms (audio::OutputGain). They survive seeks,
    // pauses and device changes.
    void SetVolume(float volume) noexcept {
        const float most = audio::OutputGain::kMaxVolume;
        dsp_controls_.volume.store(volume > most ? most : volume > 0 ? volume : 0.0f, std::memory_order_relaxed);
    }
    float Volume() const noexcept { return dsp_controls_.volume.load(std::memory_order_relaxed); }
    float VolumeDecibels() const noexcept { return audio::OutputGain::Decibels(Volume()); }
    void SetMuted(bool muted) noexcept { dsp_controls_.muted.store(muted, std::memory_order_relaxed); }
    bool Muted() const noexcept { return dsp_controls_.muted.load(std::memory_order_relaxed); }
    const audio::OutputGain::Statistics& GainStats() const noexcept { return output_gain_.Stats(); } // Read after Run.
    unsigned ValidatedFrames() const noexcept { return validated_frames_.load(); }
    // Pre-roll: from Run() to the start of the clock, in 100 ns (0: it never started). FirstFrameTime() is the picture.
    // The longest single call of each kind on the presentation thread while the clock ran (100 ns), and the picture
    // on screen when it happened: a flip cannot be on time if the thread that submits it is inside another call.
    struct Stalls {
        std::int64_t convert = 0, convert_pts = 0, present = 0, present_pts = 0, messages = 0, messages_pts = 0;
        std::int64_t convert_sum = 0, present_sum = 0, messages_sum = 0; std::uint32_t converts = 0, presents = 0, passes = 0; // Totals while the clock runs.
    };
    const Stalls& LongestCalls() const noexcept { return stalls_; }
    // Low-power pacing (PlaybackOptions::low_power_pacing). CPU time (user + kernel, GetThreadTimes) of the presentation
    // thread, the media worker and the whole process, and wall time, while playing and while paused, from the clock's start
    // to the end of Run. The presentation thread's waits: how many, how long in all, how many ended before their timer (a
    // message, a decoded picture, a GPU fence); for the waits before a picture's deadline, how late the timer woke it (from
    // the time asked; late_*: over 1 ms, over 4 ms). The worker's waits (passes that changed nothing). 100 ns. Read after Run.
    struct Pacing {
        struct Share { std::int64_t wall = 0, presentation = 0, worker = 0, process = 0; };
        Share playing{}, paused{};
        std::uint32_t waits = 0, early = 0, timed = 0, late_1ms = 0, late_4ms = 0;
        std::int64_t waited = 0, late_sum = 0, late_max = 0;
        std::uint32_t worker_waits = 0; std::int64_t worker_waited = 0;
        std::uint32_t paused_waits = 0, settled_waits = 0; // Waits while paused; of them, settled (the long sleep).
        std::uint32_t past_deadline = 0; // Timed waits that returned after the picture's deadline itself (not just past the spin).
        std::int64_t past_sum = 0, past_max = 0; // How far past it (100 ns).
        // Passes that spun instead, by why: pacing off (or no timer); something in motion (the start, a seek landing, steps,
        // a recovery, a resize); a decoded picture to convert or retire now; Present to call again at once (a picture
        // composited, due or untimed, an image to retry); a picture due within kPaceFloor; the controls due within half a spin.
        enum class Spin : std::uint8_t { Off, Moving, Convert, Present, Deadline, Controls, Count };
        std::uint64_t spins[static_cast<unsigned>(Spin::Count)]{};
    };
    const Pacing& PacingStats() noexcept { pacing_.worker_waits = worker_waits_; pacing_.worker_waited = worker_waited_; return pacing_; }
    static constexpr std::int64_t kPaceSpin = 12000;  // The controls' coarse margin (half of it), the old picture spin (1.2 ms).
    std::int64_t pace_spin_ = 4500, pace_floor_ = 6750; // PlaybackOptions::pace_spin_us, and 1.5x it.
    static constexpr std::int64_t kPaceFloor = 20000; // A picture due sooner than this (2 ms) is spun for at once.
    std::uint32_t PrerollPictures() const noexcept { return preroll_pictures_.load(std::memory_order_relaxed); } // Pictures to show decoded before the clock started.
    std::int64_t ClockStart() const noexcept { const auto at = clock_started_.load(std::memory_order_acquire); return at ? at - run_began_ : 0; }
    unsigned Error() const noexcept { return error_.load(); }
    HRESULT NativeError() const noexcept { return native_error_.load(); }
    bool Hevc() const noexcept { return hevc_; }
    bool SoftwareVideo() const noexcept { return av1_; } // API 17: AV1 through the platform's software decoder (ENGINE_MANIFESTO.md 2E).
    demux::ContainerFormat Container() const noexcept { return parser_.Format(); }
    const PictureDescription& Picture() const noexcept { return picture_; }
    bool TenBit() const noexcept { return ten_bit_; }
    unsigned Width() const noexcept { return width_; }
    unsigned Height() const noexcept { return height_; }
    // Subtitle outcome of Prepare: Off (none requested), Ready, AtlasExhausted (a hard limit hit: glyphs left out) or Disabled (failed,
    // with SubtitleError() as the native cause). Never a playback failure.
    render::SubtitleStatus SubtitleState() const noexcept {
        return subtitle_state_ == render::SubtitleStatus::Ready ? renderer_.Subtitles().Status() : subtitle_state_;
    }
    HRESULT SubtitleError() const noexcept { return subtitle_error_; }
    std::int64_t SubtitlePrepareTime() const noexcept { return subtitle_prepare_; } // 100 ns spent preparing the subtitle pipeline.
    std::int64_t FirstFrameTime() const noexcept { return first_frame_; }         // From Run to the first presented frame (100 ns).
    // Where Prepare's time went (100 ns, each stage from the end of the previous one): the arena, opening the source, the
    // container's metadata and track catalogue, the renderer (device, window, swap chain, pipelines), subtitles and their
    // search indexes, chapters, the decoder, the worker's audio (decoders and endpoint), and sealing.
    struct PrepareTiming { std::int64_t arena = 0, open = 0, metadata = 0, renderer = 0, subtitles = 0, timeline = 0, decoder = 0, audio = 0, total = 0; };
    const PrepareTiming& PrepareTimes() const noexcept { return prepare_times_; }
    const render::D3D12Renderer& Renderer() const noexcept { return renderer_; }
    // The event camera in the window (2026-11-03; D3D12Renderer::SetEventCamera): any thread between Prepare and Close; the
    // events from one consumer thread. False / 0: none (no telemetry module, no video).
    bool SetEventCamera(const video::EventCameraOptions* options) noexcept { return renderer_.SetEventCamera(options); }
    bool SetStabilization(bool on) noexcept { return renderer_.SetStabilization(on); } // The view (2026-11-04); any thread between Prepare and Close.
    std::uint32_t PollEvents(video::EventSpike* out, std::uint32_t capacity) noexcept { return renderer_.Events().Configured() ? renderer_.Events().Poll(out, capacity) : 0; }
    [[nodiscard]] render::D3D12Renderer::EventCameraState EventCamera() const noexcept { return renderer_.EventState(); }
    // Tensor export in the window (2026-11-03; D3D12Renderer::RequestTensor): the region (D3D12Renderer::TensorKind; `fractions`
    // left, top, right, bottom for kTensorCustom) of the next picture converted, written into `out` as `spec` says
    // (video::ExportTensorMeans) on the calling thread (any but the presentation thread). 1: written (`answer` says what was
    // taken); 0: no picture converted within `timeout_ms` (paused, not playing yet); -1: refused (another request, too large,
    // a region off the picture, a spec `out` cannot hold).
    int ExportTensor(const video::TensorSpec& spec, std::uint32_t kind, const float* fractions, void* out, std::size_t bytes,
                     render::D3D12Renderer::TensorAnswer& answer, std::uint32_t timeout_ms) noexcept;
    // Scripted resize test: one record per step (read after Run).
    struct ResizeStep {
        unsigned requested_width = 0, requested_height = 0; // Client size asked for (0 = fullscreen toggle).
        unsigned width = 0, height = 0;                     // Back buffer after the step was applied.
        render::D3D12Renderer::VideoRect video{};           // Letterboxed video in the first composite afterwards.
        unsigned presented_before = 0, presented_after = 0; // Video frames presented when applied / by the next step.
        float hud_scale = 0, dock_left = 0, dock_bottom = 0;
        bool applied = false, fullscreen = false;
    };
    unsigned ResizeSteps() const noexcept { return resize_steps_; }
    const ResizeStep& ResizeAt(unsigned i) const noexcept { return resize_[i < 6 ? i : 5]; }
    // GPU device loss, absorbed without stopping playback (read after Run). Whichever thread first sees the device
    // removed stops using it; the presentation thread rebuilds renderer and decoder on a new device and decoding
    // restarts within the same playback epoch at the picture on screen (paused, or a seek still landing) or at the
    // clock's media time (playing). Audio and its clock run on untouched; video catches up with them.
    struct DeviceRecovery {
        HRESULT reason = S_OK;       // GetDeviceRemovedReason of the lost device (else the failing call's result).
        unsigned detector = 0;       // 1: presentation thread (Present, Convert or the health probe), 2: media worker.
        unsigned attempts = 0;       // Rebuild attempts (a restarting driver may refuse the first ones).
        ULONG leaked = 0;            // References left on the lost device once everything was released (must be 0).
        bool paused = false;         // Paused when the loss was noticed.
        std::int64_t lost = 0, rebuilt = 0; // Now() when noticed and when the new device was ready.
        // Now() once the worker had parked, once the lost device was released, and once the renderer was rebuilt (the
        // decoder follows, until `rebuilt`).
        std::int64_t parked = 0, released = 0, recreated = 0;
        std::int64_t shown = 0;      // PTS on screen when noticed (INT64_MIN: none yet).
        std::int64_t target = 0;     // Media time decoding resumed at (INT64_MIN: the epoch's own target).
        unsigned subtitle_presents = 0; // Subtitle composites before the rebuild (later ones prove the new atlas).
    };
    unsigned DeviceRecoveries() const noexcept { return device_recoveries_; }
    const DeviceRecovery& LastDeviceRecovery() const noexcept { return recovery_; }
    const render::D3D12Renderer::SeekPresentation& RecoveryLanding() const noexcept { return renderer_.RecoveryLanding(); }
    // Audio endpoint faults absorbed by the sink (read after Run): see Win32WasapiAudioSink.
    const platform::windows::EndpointRecovery& AudioRecovery() const noexcept { return audio_recovery_; }
    // Any thread, fault tests: the media worker passes this to its sink's InjectInvalidation once the endpoint has
    // started (the stream is invalidated with `loss`, and endpoints refuse to open for `refuse_ms`). Recovery then runs
    // exactly as for a real loss, without taking the machine's audio device from other applications. Lock-free.
    void InjectAudioFault(HRESULT loss, std::uint32_t refuse_ms) noexcept {
        audio_fault_.store(static_cast<std::uint64_t>(refuse_ms) << 32 | static_cast<std::uint32_t>(loss), std::memory_order_release);
    }
    bool AudioAttached() const noexcept { return audio_attached_; } // False: the null endpoint was playing at the end.
    // Clock continuity over the `AudioHandovers()` re-anchors that followed an endpoint change, compared with the old
    // endpoint's clock (its last prediction, run on at 1x), in 100 ns:
    // - skip: media ahead of that prediction: PCM lost with a dead endpoint's buffer (never replayed);
    // - hold: media behind it, still after what had been reached: the timeline waited (a starting endpoint);
    // - repeat: media before what the old clock had already reached (must stay 0).
    struct AudioContinuity { unsigned handovers = 0; std::int64_t skip = 0, hold = 0, repeat = 0; };
    const AudioContinuity& Continuity() const noexcept { return continuity_; }
    // ---- Chapters and scenes (docs/chapters.md) ----
    // The file's chapters (Nero chpl, else a QuickTime chapter track) or the sections synthesized from its structure, and
    // its scenes: structural and dialogue marks from Prepare, and cuts seen while playing. UI thread after Prepare.
    const timeline::ChapterTimeline& Timeline() const noexcept { return timeline_; }
    const timeline::SynthesisResult& Synthesis() const noexcept { return synthesis_; }
    std::int64_t TimelinePrepareTime() const noexcept { return timeline_prepare_; } // 100 ns.
    // UI thread: the next (direction > 0) or previous chapter or scene from the position, as a seek with a status cue
    // ("Chapter 3 · Finale · 02:15", "Section 2 · 10:00", "Scene 7 · 01:02:03"). NotFound: none that way (a warning cue).
    // Page Down and Page Up do it for chapters.
    ai::IntentStatus Navigate(bool chapter, int direction) noexcept;
    ai::IntentStatus ReplayCue() noexcept; // The cue on screen (or just ended) from its start.
    std::int64_t Position() const noexcept; // Where playback is, or the target of a seek still on its way.
    struct NavigationResult {
        bool chapter = false; int direction = 0; ai::IntentStatus status = ai::IntentStatus::Ok;
        std::int64_t from = 0, target = 0; unsigned index = 0; std::uint32_t epoch = 0; // The seek it started (0: none).
        char cue[ui::StatusCue::kMaxBytes]{};
    };
    static constexpr unsigned kMaxNavigations = 16;
    unsigned Navigations() const noexcept { return navigation_count_; }
    const NavigationResult& NavigationAt(unsigned i) const noexcept { return navigations_[i < kMaxNavigations ? i : kMaxNavigations - 1]; }
    // --test-quality: when the script changed the load and the mode (Now(), 100 ns).
    struct QualityScript { std::int64_t load_on = 0, load_off = 0, auto_again = 0; };
    const QualityScript& QualityRun() const noexcept { return quality_run_; }
    // ---- A–B loop (docs/seeking.md, "A–B loop") ----
    // UI thread. The L key: A at the position, then B (the loop [A, B] plays on, turning back to A at B), then off. The
    // commands: either end, off, or the sentence on screen (or just said) padded 150 ms on both sides. Each change shows a
    // status cue and the bracket on the dock's timeline. Turning back is a seek to A as soon as the clock reaches B.
    ai::IntentStatus ToggleLoop() noexcept;
    ai::IntentStatus SetLoopStart() noexcept;
    ai::IntentStatus SetLoopEnd() noexcept;
    ai::IntentStatus ClearLoop() noexcept;
    ai::IntentStatus LoopCue() noexcept;
    const timeline::LoopRegion& Loop() const noexcept { return loop_; }
    // Every turn (the first kMaxTurns): the clock's media time when B was seen (overshoot past B), the request-to-screen
    // latency of the seek to A, and the picture it landed on.
    struct LoopTurn { std::int64_t a = 0, b = 0, seen = 0, requested = 0, latency = 0, pts = 0, duration = 0; std::uint32_t epoch = 0; bool landed = false, cue = false, pinned = false;
                      std::int64_t audio = INT64_MIN, resumed = 0; std::uint8_t waited = 0; }; // The first PCM frame's media time; request to the clock's anchor (100 ns).
    static constexpr unsigned kMaxTurns = 16;
    struct LoopRecord { LoopTurn turns[kMaxTurns]{}; unsigned count = 0; bool done = false; std::int64_t cue_start = 0, cue_end = 0; unsigned cue_index = 0; };
    const LoopRecord& LoopRun() const noexcept { return loop_run_; }
    // ---- Telemetry (docs/session.md, "Flight recorder", "Telemetry readout") ----
    // Two bounded flight recorders (core::FlightRecorder, 256 entries each, no allocation): what the presentation thread
    // and the media worker did. Any thread may read them.
    using Recorder = core::FlightRecorder<256>;
    const Recorder& UiEvents() const noexcept { return ui_events_; }
    const Recorder& WorkerEvents() const noexcept { return worker_events_; }
    // UI thread: the engine's telemetry now, from its own state (the newest events of both recorders, up to kTelemetryEvents).
    static constexpr unsigned kTelemetryEvents = 16;
    ui::TelemetrySnapshot Telemetry() noexcept;
    // UI thread. The I key toggles the panel (refreshed every second while it is up); `explain`: why the quality is what it is,
    // once. "show stats", "status report", "הצג נתונים", "why did quality change?", "למה האיכות ירדה?".
    ai::IntentStatus ShowStats(bool show, bool explain = false) noexcept;
    bool StatsShown() const noexcept { return stats_shown_; }
    // --test-telemetry's record: each readout's text as shown, and the telemetry it was made from.
    struct TelemetryReadout { char text[ui::StatusCue::kMaxBytes]{}; ui::TelemetrySnapshot snapshot{}; bool explain = false; };
    static constexpr unsigned kMaxReadouts = 6;
    struct TelemetryRecord { TelemetryReadout readouts[kMaxReadouts]{}; unsigned count = 0; std::int64_t load_on = 0; bool done = false; };
    const TelemetryRecord& TelemetryRun() const noexcept { return telemetry_run_; }
    // Liveness (docs/session.md, "Watchdog"): how far each thread has got. A test harness's watchdog reads these from its
    // own thread to tell a slow run from a stuck one, and where it is stuck.
    enum class Phase : std::uint32_t { Idle, Preparing, Messages, Stream, Scripts, Stopping, Closing, WorkerStart, WorkerLoop, WorkerControls,
                                       WorkerSwitch, WorkerSeek, WorkerMedia, WorkerStopped, WorkerTeardown, Joined, Convert, Present };
    struct Liveness { std::uint64_t ui_beats = 0, worker_beats = 0; Phase ui = Phase::Idle, worker = Phase::Idle; };
    Liveness Alive() const noexcept {
        return {ui_beats_.load(std::memory_order_relaxed), worker_beats_.load(std::memory_order_relaxed),
                static_cast<Phase>(ui_phase_.load(std::memory_order_relaxed)), static_cast<Phase>(worker_phase_.load(std::memory_order_relaxed))};
    }
    static const char* Name(Phase) noexcept;
    // ---- Modules and frame signatures (docs/sdk.md) ----
    std::uint32_t Modules() const noexcept { return options_.modules; }
    core::ArenaStatistics ArenaStats() const noexcept { return arena_.Statistics(); }
    // An enabled module's region (ModuleIndex order): its capacity and what the module used of it. Zero for one left out.
    core::ArenaStatistics ModuleArenaStats(unsigned index) const noexcept {
        return index < kModuleCount && module_used_[index] ? module_arenas_[index].Statistics() : core::ArenaStatistics{};
    }
    std::uint64_t ModuleArenaBytes() const noexcept { return ArenaBudget(options_.modules) - kCoreArenaBytes; }
    // Threads this coordinator (and its renderer) started for the media: the worker, the file reader, voice, the command bar's
    // and the status cues' text threads, the subtitle shaper. After Prepare.
    unsigned Threads() const noexcept;
    // The custom wake word's templates the recogniser holds (0: the built-in "Avion", or a name heard as text only), and of
    // them those of a speech source (the host's or the system's voices).
    [[nodiscard]] unsigned WakeTemplates() const noexcept { return wake_templates_; }
    [[nodiscard]] unsigned WakeSourceTemplates() const noexcept { return wake_source_templates_; }
    // The oldest signature not taken yet (kModuleTelemetry). One consumer, any thread.
    bool PollFrame(FrameSignature& out) noexcept;
    std::uint32_t Signatures() const noexcept { return signature_count_.load(std::memory_order_relaxed); }
    std::uint32_t SignaturesLost() const noexcept { return signatures_lost_.load(std::memory_order_relaxed); }
    // Any thread: Run ends at its next pass (as the window closing does).
    void RequestStop() noexcept { stop_requested_.store(true, std::memory_order_relaxed); WakePresenter(); }
    // Embedding (API 12; docs/sdk.md, "Embedding: API 12"). The host's thread (one producer): a command for Run's next pass, which
    // is woken now (paused too). False: too long or the ring (32) is full.
    bool PostCommand(std::string_view utf8, std::uint32_t id) noexcept;
    // API 15: a structured control (HostControl.hpp; its value already checked), on the same ring and with the same kCommand event.
    bool PostControl(HostControl op, double value, std::uint32_t id) noexcept;
    // Run's thread: a control at once, as the intent its command would have become (a seek past either end lands on it).
    ai::IntentStatus Control(HostControl op, double value) noexcept;
    // One consumer thread: the oldest event (kOpened, kState, kPosition, kSeekLanded, kCommand, kFullscreen, kEnded).
    bool PollEvent(HostEvent& out) noexcept;
    // Any thread: what Run's last pass published.
    EmbedState PublishedState() const noexcept;
    // Texture output (API 13): the host's renderer thread (one), through the renderer; a new size from the next pass.
    bool OutputTextures(render::D3D12Renderer::OutputDesc& out) const noexcept { return renderer_.OutputTextures(out); }
    bool AcquireOutput(render::D3D12Renderer::OutputFrame& out) noexcept { return renderer_.AcquireOutput(out); }
    bool ReleaseOutput(unsigned index, std::uint64_t host_value) noexcept { return renderer_.ReleaseOutput(index, host_value); }
    void ResizeOutput(unsigned width, unsigned height) noexcept { renderer_.RequestOutputSize(width, height); WakePresenter(); }
    bool TextureOutput() const noexcept { return renderer_.TextureOutput(); }
    // UI thread: an utterance recognised by the host, through the wake gate (name, window, push-to-talk) to Command.
    // False: the gate set it aside. The decision is recorded like a voice result.
    bool Hear(std::string_view utf8) noexcept;
    void SetTalk(bool held) noexcept { wake_gate_.SetTalk(held); text_gate_.SetTalk(held); }
    // The wake policy now: `required` false is no name (push_to_talk: only while talk is held); `word` empty or null: "Avion".
    bool SetWake(bool required, bool push_to_talk, const char* word) noexcept;
    unsigned WakeIgnored() const noexcept { return wake_gate_.Stats().ignored + text_gate_.Stats().ignored; }
    // ---- Audio zoom, picture controls, micro-motion and cures (docs/audio-dsp.md, docs/picture.md, docs/intents.md) ----
    // UI thread. "audio zoom", "זום אקוסטי": the picture's zoom steers the sound (audio::AudioFocus); its state follows the
    // renderer's zoom every UI pass.
    bool SetAudioZoom(bool on, bool cue = true) noexcept;
    bool AudioZoom() const noexcept { return dsp_controls_.audio_zoom.load(std::memory_order_relaxed); }
    // "micro motion", "tactical vision", "הדגש תנועה זעירה": micro-motion magnification at kMicroGain (Reconstruct.hlsli).
    static constexpr float kMicroGain = 6.0f;
    bool SetMicroMotion(bool on, bool cue = true) noexcept;
    bool MicroMotion() const noexcept { return renderer_.PictureControlsNow().micro_motion > 1; }
    // Everything the cures touch, as it is now, and set at once.
    ui::PictureAudioCures::Settings CurrentSettings() const noexcept;
    void ApplySettings(const ui::PictureAudioCures::Settings&) noexcept;
    // "too dark" / "חשוך", "noisy" / "יש זמזום", "muffled" / "הדיבור נבלע"; "undo" / "בטל" right after puts things back.
    ai::IntentStatus Cure(ai::IntentKind kind) noexcept;
    ai::IntentStatus UndoCure() noexcept;
    const ui::PictureAudioCures& Cures() const noexcept { return cures_; }
    // What the DSP's analysis hears now (published by the worker per block): speech probability, dominant band and its level.
    float SpeechNow() const noexcept { return speech_now_.load(std::memory_order_relaxed); }
    unsigned DominantBandNow() const noexcept { return band_now_.load(std::memory_order_relaxed); }
    unsigned NotchesNow() const noexcept { return notches_now_.load(std::memory_order_relaxed); }
    // ---- Night / smart loudness mode (docs/audio-dsp.md, "Loudness and night mode") ----
    // UI thread (any thread for the flag). K, "night mode", "מצב לילה": quiet speech lifted toward -20 LUFS, loud bursts pulled
    // down, the true peak limited at -1 dBTP; a status cue says it. The meters run always (LoudnessNow).
    bool SetNight(bool on, bool cue = true) noexcept;
    bool Night() const noexcept { return dsp_controls_.night.load(std::memory_order_relaxed); }
    struct LoudnessReading { float momentary = -144, short_term = -144, true_peak = -144, gain = 0, limiter = 0; };
    LoudnessReading LoudnessNow() const noexcept; // Published by the media worker every 100 ms of PCM.
    // ---- Bookmarks and the pinned frame (docs/session.md, "Bookmarks"; docs/picture.md, "Pinned | Live") ----
    // UI thread. B, "add bookmark", "סמן הערה": a bookmark at the position (32 at most, half a second apart; kept in the
    // resume ledger with the file), and the picture on screen pinned as the reference frame. Ctrl+B, "pin frame": the frame
    // alone. "go to bookmark 2", "next bookmark", "לך לסימנייה": seeks. V cycles Original | Enhanced, Pinned | Live, off.
    ai::IntentStatus AddBookmark() noexcept;
    ai::IntentStatus GoToBookmark(unsigned number) noexcept; // From 1; 0: the next one.
    ai::IntentStatus NextBookmark() noexcept;
    ai::IntentStatus PreviousBookmark() noexcept;
    ai::IntentStatus PinFrame() noexcept;
    ai::IntentStatus ComparePinned() noexcept;
    const session::Bookmarks& FileBookmarks() const noexcept { return bookmarks_; }
    struct BookmarkRecord {
        std::int64_t added[3]{};                     // The bookmarks the script made.
        struct Jump { unsigned number = 0; std::int64_t target = 0, pts = 0, duration = 0, latency = 0; bool landed = false; std::uint32_t epoch = 0; };
        Jump jumps[5]{}; unsigned jump_count = 0;
        std::int64_t pinned = INT64_MIN, live = INT64_MIN;     // The reference frame's and the live frame's PTS.
        render::D3D12Renderer::Capture held{}, reference{}, screen{};
        bool done = false;
    };
    const BookmarkRecord& BookmarkRun() const noexcept { return bookmark_run_; }
    // The execution state this thread held while playing, as Windows reported it when the last stretch of playing ended
    // (ES_SYSTEM_REQUIRED, and ES_DISPLAY_REQUIRED with a picture; docs/session.md, "Staying awake"). 0: never released.
    std::uint32_t AwakeHeld() const noexcept { return awake_held_; }
private:
    // One streaming step on the UI thread: convert at most one decoded frame and present. False on failure.
    bool Stream(bool& finished) noexcept;
    static void Pump(void* context) noexcept;
    static DWORD WINAPI WorkerEntry(void*) noexcept;
    static DWORD WINAPI VoiceEntry(void*) noexcept;
    void VoiceThread() noexcept;
    void DrainVoice() noexcept; // UI thread.
    static void HandleWake(void* context, ai::WakeDecision, std::string_view command, const audio::Utterance&) noexcept;
    static bool HandleHud(void*, ui::HudAction) noexcept;
    static ai::IntentStatus HandleIntent(void*, const ai::Intent&) noexcept;
    static bool HandlePaletteSubmit(void*, std::string_view) noexcept;
    static ui::PaletteHint HandlePalettePreview(void*, std::string_view, unsigned& matches) noexcept;
    void Worker() noexcept;
    bool Configuration(const demux::TrackInfo&, bool video, std::byte* asc = nullptr, unsigned* asc_bytes = nullptr) noexcept;
    std::string_view ImportText(const wchar_t* path, unsigned parser_index) noexcept; // Preparing: tx3g samples as WebVTT.
    // UI thread: a transient status cue on the HUD, held `hold_seconds` once shown.
    void Status(const char* utf8, ui::StatusIcon, ui::StatusTone, float hold_seconds = ui::StatusCue::kHoldSeconds) noexcept;
    void TrackCue(const MediaTrack*, bool audio) noexcept;
    ai::IntentStatus SelectTracks(const ai::Intent&) noexcept;
    std::int64_t Now() const noexcept;
    void Fail(unsigned code, HRESULT hr = S_OK) noexcept { native_error_.store(hr); error_.store(code); }
    // Device-loss handshake. Running -> Lost (either thread; compare-exchange) -> Parked (worker: no more decoder
    // use, DPB dropped) -> Resume (presentation thread: device, renderer and decoder rebuilt) -> Running (worker:
    // demux repositioned). Each store is a release that hands the decoder to the other thread.
    enum : std::uint32_t { kGpuRunning, kGpuLost, kGpuParked, kGpuResume };
    void LoseGpu(unsigned detector, HRESULT) noexcept;
    bool RecoverGpu() noexcept; // Presentation thread, while the phase is not Running.
    std::atomic<std::uint32_t> gpu_phase_{kGpuRunning}, gpu_detector_{0};
    std::atomic<HRESULT> gpu_reason_{S_OK};
    std::atomic<std::int64_t> recovery_target_{INT64_MIN};
    std::atomic<bool> recovery_catch_up_{false}; // Target taken from the running clock: follow it while decoding.
    DeviceRecovery recovery_{};
    unsigned device_recoveries_ = 0;
    bool recovering_ = false;
    std::int64_t recovery_deadline_ = 0, recovery_retry_ = 0;
    platform::windows::EndpointRecovery audio_recovery_{}; // Worker-written, read after Run.
    std::atomic<std::uint64_t> audio_fault_{0}; // InjectAudioFault: refuse_ms << 32 | loss (0: none pending).
    bool audio_attached_ = true;
    AudioContinuity continuity_{}; // Worker-written, read after Run.
    QualityScript quality_run_{};  // UI thread.
    // A–B loop (UI thread).
    timeline::LoopRegion loop_;
    LoopRecord loop_run_{};
    unsigned loop_script_ = 0; std::int64_t loop_mark_ = 0;
    std::uint32_t loop_epoch_ = 0; // The newest turn's seek (0: none landing).
    bool SendPinnedSeek(std::int64_t a, std::int64_t end) noexcept; // A turn through the pinned pictures.
    // The worker's pinned loop PCM (docs/seeking.md, "Pinned loop turnaround"): from the first frame at A, kLoopPcm of it,
    // published once whole (anchor last, release).
    static constexpr std::int64_t kLoopPcm = 5000000; // 500 ms.
    float* loop_pcm_ = nullptr; unsigned loop_pcm_capacity_ = 0, loop_pcm_channels_ = 0;
    std::atomic<std::int64_t> loop_pcm_anchor_{INT64_MIN}, loop_pcm_origin_{0}, loop_pcm_end_{0};
    // Night mode's meters, published by the worker.
    std::atomic<float> lufs_momentary_{-144.0f}, lufs_short_{-144.0f}, true_peak_{-144.0f}, night_gain_{0.0f}, limiter_db_{0.0f};
    // Bookmarks (UI thread).
    session::Bookmarks bookmarks_;
    void BookmarksChanged() noexcept; // The dock's flags.
    void BookmarkCue(const char* what, unsigned number, std::int64_t at) noexcept;
    BookmarkRecord bookmark_run_{};
    unsigned bookmark_script_ = 0; std::int64_t bookmark_mark_ = 0;
    void PumpLoop(bool finished) noexcept;
    static constexpr std::int64_t kStageAhead = 1500000; // A pinned turn's landing is staged this long before B (150 ms).
    void LoopChanged() noexcept;       // The dock's bracket, the flight recorder.
    void LoopCueText(timeline::LoopRegion::Step) noexcept;
    std::int64_t PrecisePosition() const noexcept; // The clock's media time while playing; else Position().
    // Telemetry (UI thread unless noted).
    Recorder ui_events_, worker_events_; // worker_events_: written by the media worker only.
    core::FlightRecord merged_[kTelemetryEvents]{}, scratch_a_[kTelemetryEvents]{}, scratch_b_[kTelemetryEvents]{};
    bool stats_shown_ = false;
    std::int64_t stats_refresh_ = 0, stats_shown_at_ = 0;
    std::uint32_t stats_revision_ = 0; // The status cue revision of the panel (another cue ends the refresh).
    TelemetryRecord telemetry_run_{};
    unsigned telemetry_script_ = 0; std::int64_t telemetry_mark_ = 0;
    std::uint32_t noted_landing_ = 0; unsigned noted_changes_ = 0, noted_drops_ = 0, noted_recoveries_ = 0; float noted_zoom_ = 1, noted_zoom_x_ = 0.5f, noted_zoom_y_ = 0.5f;
    void NoteEvents() noexcept;  // What the renderer did since the last look, into ui_events_.
    void PumpStats() noexcept;
    void RecordReadout(bool explain) noexcept;
    std::atomic<std::uint64_t> ui_beats_{0}, worker_beats_{0};
    std::atomic<std::uint32_t> ui_phase_{0}, worker_phase_{0};
    void UiPhase(Phase p) noexcept { ui_phase_.store(static_cast<std::uint32_t>(p), std::memory_order_relaxed); }
    // Staying awake: while playing, the system (and with a picture, the display) does not sleep. UI thread.
    void KeepAwake(bool playing) noexcept;
    bool awake_ = false;
    // Low-power pacing: the end of each presentation pass (Pace), the worker's word that a picture is decoded (WakePresenter),
    // and the CPU shares (AccountCpu: the time since the last call to the state before it).
    void Pace() noexcept;
    void WakePresenter() noexcept;
    void WakeWorker() noexcept;      // UI thread: a command committed, a picture returned.
    bool CommitControl() noexcept;   // controls_->CommitWriteSlot, then WakeWorker.
    HANDLE worker_timer_ = nullptr, worker_wake_ = nullptr;
    std::atomic<bool> worker_waiting_{false};
    std::uint32_t worker_waits_ = 0; std::int64_t worker_waited_ = 0; // Worker; read after Run.
    void AccountCpu() noexcept;
    HANDLE pace_timer_ = nullptr, pace_picture_ = nullptr, pace_fence_ = nullptr;
    std::atomic<bool> pace_waiting_{false};
    std::int64_t script_wake_ = INT64_MAX; // The scripts' next deadline (Now() units), for a settled pause's wait.
    Pacing pacing_{};
    bool cpu_counting_ = false;
    std::int64_t cpu_wall_ = 0, cpu_presentation_ = 0, cpu_worker_ = 0, cpu_process_ = 0;
    std::uint32_t awake_held_ = 0, awake_prior_ = ES_CONTINUOUS; // The thread's own request before playing (restored).
    void WorkerPhase(Phase p) noexcept { worker_phase_.store(static_cast<std::uint32_t>(p), std::memory_order_relaxed); }
    WarmSession* warm_ = nullptr;
    core::LinearMemoryArena own_arena_;
    core::LinearMemoryArena& arena_; // The warm session's file arena, or own_arena_.
    // Byte source: a local file through IOCP, or an http(s):// URL through HttpBlockSource. Same contract.
    platform::windows::Win32IocpBlockSource file_;
    platform::windows::HttpBlockSource http_;
    platform::IBlockSource* source_ = &file_;
    bool remote_ = false;
    void PumpSource() noexcept { if (remote_) (void)http_.Pump(); else (void)file_.Pump(); }
    demux::Demuxer parser_; // ISO BMFF or Matroska (API 17).
    // AV1 (API 17, ENGINE_MANIFESTO.md 2E): the platform's software decoder, its pictures uploaded into the decoder's surfaces
    // (D3D12VideoDecoder's VideoCodec::Upload). Prepared and used by the worker.
    platform::windows::Win32VideoMftDecoder av1_decoder_;
    platform::windows::UploadPicture av1_picture_{};
    bool av1_ = false;
    codec::H264SyntaxParser syntax_;
    codec::HevcSyntaxParser hevc_syntax_;
    codec::H264Dxva* dxva_ = nullptr;
    codec::HevcDxva* hevc_dxva_ = nullptr;
    render::D3D12Renderer renderer_; // With the warm session's GpuSession, when there is one.
    subtitle::SubtitleTrack subtitles_;
    subtitle::SearchIndex search_;   // Immutable after Prepare.
    // API 17: subtitles after open. The sources made at open (the sidecar's place first, then the file's tracks), the slot an
    // attached file takes, two states and their arenas (views of the subtitle module's region), and what is live.
    std::string_view open_sources_[subtitle::SubtitleTrack::kMaxChannels]{}; unsigned open_source_count_ = 0, attached_slot_ = 0;
    subtitle::SubtitleTrack attached_[2]; subtitle::SearchIndex attached_search_[2];
    core::LinearMemoryArena attach_read_, attach_states_[2];
    unsigned attach_next_ = 0; bool attach_ready_ = false, attach_broken_ = false;
    std::atomic<bool> attaching_{false}, attach_choice_{false};
    std::atomic<const subtitle::SubtitleTrack*> live_track_{&subtitles_};
    std::atomic<const subtitle::SearchIndex*> live_search_{&search_};
    ai::ConceptIndex concepts_;      // Immutable after Prepare.
    // Sessions: this file's key and size in the ledger (0: not a local file), the resume record, a file asked for.
    std::uint64_t resume_key_ = 0, resume_bytes_ = 0;
    ResumeRecord resume_{};
    FileRequest file_request_ = FileRequest::None;
    char file_name_[96]{}; // This file's name (UTF-8), for the start's cue.
    void StartCue() noexcept;
    void Remember() noexcept;
    SubtitleSeek subtitle_seek_{};
    std::int64_t search_prepare_ = 0;
    ai::LocalIntentDispatcher dispatcher_;
    ai::ConversationState conversation_;
    NeuralReuseRun neural_reuse_run_{};
    unsigned paused_refreshes_ = 0;
    CommandResult commands_[kMaxCommands]{};
    unsigned command_results_ = 0;
    std::int64_t seek_target_ = 0; // UI-owned: the newest seek's target.
    render::SubtitleStatus subtitle_state_ = render::SubtitleStatus::Off;
    HRESULT subtitle_error_ = S_OK;
    std::int64_t subtitle_prepare_ = 0, first_frame_ = 0;
    PrepareTiming prepare_times_{};
    ResizeStep resize_[6]{};
    unsigned resize_steps_ = 0;
    bool streaming_ = false; // Reentrancy guard: the modal-loop pump runs inside Messages().
    platform::windows::D3D12VideoDecoder own_decoder_;
    platform::windows::D3D12VideoDecoder& decoder_; // The warm session's, or own_decoder_.
    clock::PredictiveMasterClock clock_;
    audio::DspControls dsp_controls_;
    audio::DspProcessor dsp_;
    audio::SpectralTracker analysis_tracker_; // The DSP's analysis (AttachAnalysis): notches, ducking, frame signatures.
    // Modules: each enabled module's region of the file arena (EngineModules.hpp); module_used_[i] while it is made.
    core::LinearMemoryArena module_arenas_[kModuleCount];
    bool module_used_[kModuleCount]{};
    core::LinearMemoryArena& ModuleArena(std::uint32_t module) noexcept { return module_arenas_[ModuleIndex(module)]; }
    bool PrepareModules() noexcept;
    // The recogniser from the voice region: the shipped templates, with a custom name's synthesised ones when it is set.
    bool ConfigureRecogniser(core::LinearMemoryArena& voice, const PlaybackOptions& options) noexcept;
    // Target tracking (docs/picture.md, "Target tracking"): T, "track target", "stop tracking".
    ai::IntentStatus Track(bool on) noexcept;
    ai::IntentStatus NextTarget() noexcept; // "next target", Tab while tracking (docs/picture.md, "Several objects at once").
    // The audio saccade (docs/audio-dsp.md, "Sound localisation and the audio saccade"): the worker's locator hears the
    // programme's front pair before the DSP; each UI pass turns the view (a live zoom, 2x from a whole view) toward a new sound.
    ai::IntentStatus Saccade(bool on) noexcept;
public:
    struct SaccadeRun { int command = -1; float right_x = -1, left_x = -1, zoom = 0; std::uint32_t saccades = 0; float right_azimuth = 0, left_azimuth = 0; };
    const SaccadeRun& SaccadeRecord() const noexcept { return saccade_run_; }
private:
    SaccadeRun saccade_run_{};
    void PumpSaccade() noexcept;
    audio::SoundLocator locator_;
    bool saccade_ = false; std::uint32_t saccade_seen_ = 0, saccades_ = 0;
    void PumpTracking() noexcept;
    std::uint32_t track_events_seen_ = 0; bool tracking_shown_ = false, track_pressed_ = false, next_target_cue_ = false;
public:
    // --test-tracking's record: each picture shown once T was pressed.
    struct TrackSample { std::int64_t pts; float shown_x, shown_y, zoom, audio_x; std::uint8_t state; std::int64_t tracked_pts; float tracked_x; float shown_zoom; };
    struct TrackRun { static constexpr unsigned kMax = 256; TrackSample samples[kMax]; unsigned count = 0; std::uint32_t events = 0; char last_cue[96]; };
    const TrackRun& TrackRecord() const noexcept { return track_run_; }
    // --test-next-target's record.
    struct TargetSnapshot {
        std::int64_t pts = -1; float shown_x = -1, shown_y = -1, zoom = 1, audio_x = -1, tracked_x = -1, tracked_y = -1; std::uint8_t state = 0;
        std::uint32_t target_id = 0, objects = 0, ids[4]{}; float x[4]{}, y[4]{};
    };
    // The first Tab (2026-11-01): the picture on screen at T and how much later the one at the Tab was, whether it moved the view
    // at once (a move counted; the view's centre then), the objects in the table and the warm grids perceived before T.
    struct TargetRun {
        static constexpr unsigned kActions = 4; TargetSnapshot after[kActions]; unsigned actions = 0, snapshots = 0; int command = -1; std::uint32_t retargets = 0;
        std::int64_t t_pts = INT64_MIN, tab_delay = -1; unsigned tab_objects = 0, warm_grids = 0; bool tab_moved = false; float tab_x = -1, tab_y = -1;
        // S at 4.2 s (2026-11-01, docs/picture.md "Forensic snapshots"): the snapshot written while tracking, the target then.
        bool snapshot_pressed = false, snapshot_written = false; std::uint32_t snapshot_target = 0, snapshot_width = 0, snapshot_height = 0; char snapshot_path[512]{};
        // The snapshot readback (2026-11-02): held before S, after the files were written, the largest made; the window then.
        std::uint64_t readback_before = 0, readback_after = 0, readback_peak = 0; unsigned window_width = 0, window_height = 0;
    };
    const TargetRun& TargetRecord() const noexcept { return target_run_; }
    struct VibrometryRun {
        struct Reading { bool taken = false; float bpm = -1, confidence = 0, seconds = 0, rate = 0; std::uint32_t readouts = 0, channel = 0; char text[64]{}; };
        Reading tracking, micro;          // At 6.0 s (T) and at 11.5 s (micro-motion).
        bool tracked = false, micro_on = false;
    };
    const VibrometryRun& VibrometryRecord() const noexcept { return vibro_run_; }
    // Forensic snapshots (docs/picture.md, "Forensic snapshots", 2026-11-01): S, "take snapshot" (nine languages) and the SDK's
    // media_engine_save_snapshot. The renderer copies the picture as composed with the next composite (D3D12Renderer::
    // RequestSnapshot); a UI pass hands the copy to the snapshot thread, which writes the BMP and its sidecar
    // (video::SaveSnapshot) to `utf8_path`, or to a free name in the snapshot folder; the UI thread neither waits nor writes.
    // Ok: on its way; NoContext: one is on its way already; NotFound: no picture yet; Unavailable: no picture (music mode);
    // Malformed: the path is too long.
    struct SnapshotRecord { std::uint32_t sequence = 0; bool written = false; std::uint32_t width = 0, height = 0; std::int64_t pts_100ns = -1; std::uint64_t hash = 0; char path[512]{}; };
    ai::IntentStatus Snapshot(const char* utf8_path = nullptr, const video::SnapshotMemory* memory = nullptr) noexcept;
    // API 17 (media_engine_snapshot_to_memory): whether the last one asked is still being made.
    [[nodiscard]] bool SnapshotBusy() const noexcept { return snapshot_phase_.load(std::memory_order_acquire) != 0; }
    // API 17 (media_engine_snapshot_to_memory), from any thread but the window's: the window's thread takes the request on its next
    // pass and makes the snapshot into `memory`; MemorySnapshotState says how it went (1 asked, 2 being made, 3 written, 4 failed);
    // ResetMemorySnapshot after reading it. False: one is already asked, or the caller is the window's thread.
    [[nodiscard]] bool RequestMemorySnapshot(const video::SnapshotMemory& memory) noexcept;
    [[nodiscard]] int MemorySnapshotState() const noexcept { return memory_request_.load(std::memory_order_acquire); }
    void ResetMemorySnapshot() noexcept { memory_request_.store(0, std::memory_order_release); }
    void SetSnapshotFolder(const char* utf8_folder) noexcept; // Null: the default.
    // The last snapshot written (or failed): false before the first and while one is on its way.
    bool LastSnapshot(SnapshotRecord& out) const noexcept;
    [[nodiscard]] std::uint32_t SnapshotsWritten() const noexcept { return snapshots_written_; }
private:
    TrackRun track_run_{};
    TargetRun target_run_{};
    VibrometryRun vibro_run_{};
    // Forensic snapshots: the thread (started with the file), its wake event and the hand-over through snapshot_phase_: 0 idle,
    // 4 the thread makes the readback (of snapshot_want_width_ x snapshot_want_height_), 5 made (snapshot_made_ says whether),
    // 1 the renderer copies, 2 the thread writes (the job: the image, its info, the path and folder; then it releases the
    // readback), 3 written (its result waits for a UI pass). The UI thread writes the job before 4 and 2 and reads the result
    // after 5 and 3; the thread the other way round.
    static DWORD WINAPI SnapshotEntry(void*) noexcept;
    void SnapshotThread() noexcept;
    void PumpSnapshot() noexcept;
    HANDLE snapshot_thread_ = nullptr, snapshot_wake_ = nullptr;
    std::atomic<int> snapshot_phase_{0};
    unsigned snapshot_want_width_ = 0, snapshot_want_height_ = 0; bool snapshot_made_ = false;
    std::atomic<bool> snapshot_stop_{false};
    char snapshot_folder_[512]{}, snapshot_media_[1024]{}, snapshot_stem_[256]{};
    char snapshot_path_[512]{}, snapshot_job_folder_[512]{}; bool snapshot_job_default_ = true; // The job's (UI thread before phase 2).
    render::D3D12Renderer::SnapshotImage snapshot_image_{};
    video::SnapshotInfo snapshot_info_{};
    video::SnapshotResult snapshot_result_{};
    video::SnapshotEncoder snapshot_encoder_;
    video::SnapshotMemory snapshot_memory_{}; bool snapshot_to_memory_ = false; // API 17.
    video::SnapshotMemory memory_ask_{}; std::atomic<int> memory_request_{0}; std::atomic<unsigned long> ui_thread_{0};
    SnapshotRecord last_snapshot_{}; bool snapshot_done_ = false; std::uint32_t snapshots_written_ = 0, snapshot_sequence_ = 0;
    std::int64_t track_last_pts_ = INT64_MIN;
    unsigned wake_templates_ = 0, wake_source_templates_ = 0;
    // Frame signatures (kModuleTelemetry): UI thread producer, one consumer (PollFrame).
    core::SpscRingBuffer<FrameSignature, 64>* signatures_ = nullptr;
    std::atomic<std::uint32_t> signature_count_{0}, signatures_lost_{0};
    static void OnSignature(void* context, const render::D3D12Renderer::PictureSignature&) noexcept;
    // Signatures waiting for the host's callback until the presentation pass is over (FlushSignatures, UI loop).
    static constexpr unsigned kPendingSignatures = 16;
    FrameSignature pending_signatures_[kPendingSignatures]{};
    unsigned pending_count_ = 0;
    void FlushSignatures() noexcept;
    // The DSP's analysis, published by the worker after each block.
    std::atomic<float> speech_now_{-1.0f}, band_db_now_{-144.0f};
    std::atomic<std::uint32_t> band_now_{10}, notches_now_{0};
    std::atomic<bool> stop_requested_{false};
    // Embedding (API 12): the host's commands and the events back, the state published every pass, and what the events compare.
    core::SpscRingBuffer<HostCommand, 32> host_commands_;
    core::SpscRingBuffer<HostEvent, 256> host_events_;
    std::atomic<std::int64_t> published_position_{0}, published_duration_{0};
    std::atomic<std::uint32_t> published_flags_{0}, published_presented_{0}, published_dropped_{0}; // flags: 1 paused, 2 muted.
    std::atomic<std::uint32_t> published_audio_track_{0}, published_subtitle_track_{UINT32_MAX}; // API 16: the choices, for any thread.
    std::atomic<float> published_volume_{0}, published_speed_{0};
    std::uint32_t events_lost_ = 0, last_seek_epoch_ = 0, fullscreen_seen_ = 0, display_width_ = 0, display_height_ = 0;
    std::int64_t last_position_event_ = 0;
    bool host_state_known_ = false, last_paused_ = false, last_muted_ = false;
    float last_volume_ = 0, last_speed_ = 0;
    void DrainHostCommands() noexcept;  // UI thread (Run): the posted commands, each answered by a kCommand event.
    void PublishHost() noexcept;        // UI thread (Run): the published state and the kState / kPosition / kSeekLanded / kFullscreen events.
    void Emit(const HostEvent& event) noexcept;
    ui::PictureAudioCures cures_;
    void SyncAudioFocus() noexcept; // UI pass: the renderer's zoom to the DSP controls.
    audio::OutputGain output_gain_; // Worker (endpoint owner) after Prepare.
    unsigned dsp_resets_ = 0;
    core::SpscRingBuffer<core::MediaPacket, 8>* packets_ = nullptr;
    core::SpscRingBuffer<core::MediaPacket, 32>* packet_returns_ = nullptr;
    core::SpscRingBuffer<core::VideoFrameDescriptor, 32>* frames_ = nullptr;
    core::SpscRingBuffer<platform::windows::VideoSurfaceReturn, 64>* surface_returns_ = nullptr;
    core::SpscRingBuffer<ControlCommand, 16>* controls_ = nullptr;
    demux::TrackInfo video_{}, audio_{}; // audio_: the track the endpoint was opened for (its format holds for all).
    PictureDescription picture_{};
    std::byte asc_[64]{};
    unsigned asc_bytes_ = 0;
    // Tracks. Prepare fills the catalogue; the worker marks audio tracks Incompatible before `ready_`.
    MediaTrack tracks_[kMaxTracks]{};
    std::uint8_t parser_index_[kMaxTracks]{};    // Catalogue -> parser track (0xff: the sidecar).
    unsigned track_count_ = 0;
    demux::TrackInfo audio_tracks_[kMaxAudioTracks]{}; // By decoder slot.
    std::byte ascs_[kMaxAudioTracks][64]{};
    unsigned asc_sizes_[kMaxAudioTracks]{}, audio_count_ = 0, first_slot_ = 0;
    std::uint32_t audio_choice_ = kNoTrack, subtitle_choice_ = kNoTrack; // UI thread.
    std::uint32_t subtitle_last_ = kNoTrack;     // UI: the last subtitle track shown ("subtitles on").
    TrackSwitch switches_[kMaxSwitches]{};
    std::atomic<unsigned> switch_count_{0};
    std::int64_t subtitle_import_ = 0;
    TrackProbe probe_{};                         // Worker-written, read after Run.
    std::atomic<unsigned> endpoint_moves_{0};    // Worker: audio endpoint changes absorbed so far.
    AudioRouting routing_{};
    std::uint32_t ignored_audio_ = 0; // Track whose packets are returned undecoded (UnsupportedLayout).
    float* fold_scratch_ = nullptr;   // Worker: one decoded span (1024 frames, up to 8 channels) before fold-down.
    // Worker: per decoder slot, its track's PCM to the output path's format (Identity() for the starting track and
    // every track of its format), and one decoded span read from a decoder before conversion.
    audio::PcmConverter converters_[kMaxAudioTracks];
    float* convert_scratch_ = nullptr;
    // Worker: pitch-preserving speed (docs/audio-dsp.md, "Playback speed"); decoded PCM goes through it while the speed is
    // not 1x, and one span of the source's PCM on its way in.
    audio::TimeStretcher stretcher_;
    float* stretch_scratch_ = nullptr;
    float speed_ = 1.0f; // UI: the speed asked for last.
    SpeedRecord speed_run_{}; // --test-speed: phases and steps (UI thread), audio (worker; read after Run).
    std::atomic<bool> speed_in_mark_{false}; // --test-speed: the PCM committed so far ends inside a mark (the worker's probe).
    SteadyRun steady_run_{}; unsigned steady_script_ = 0; // --test-stabilize.
    unsigned speed_script_ = 0;
    ZoomRecord zoom_run_{}; unsigned zoom_script_ = 0, zoom_updates_ = 0;
    unsigned stall_script_ = 0; std::int64_t stall_mark_ = 0, stall_input_mark_ = 0; // --test-window-stall.
    RotationRecord rotation_run_{}; unsigned rotation_script_ = 0, rotation_updates_ = 0, rotation_captures_ = 0, rotation_levels_ = 0; std::int64_t rotation_mark_ = 0;
    AutoHorizonRecord auto_horizon_run_{}; unsigned auto_horizon_script_ = 0, auto_horizon_captures_ = 0;
    bool music_ = false;
    core::SpscRingBuffer<audio::AudioPicture, 32>* audio_pictures_ = nullptr; // Worker -> UI thread, music mode only.
    audio::SpectrumAnalyser analyser_; // Worker-owned (BandOf is read-only after Configure).
    MusicRecord music_run_{}; unsigned music_script_ = 0; std::uint32_t music_epoch_ = 0; std::int64_t music_mark_ = 0;
    void PumpSound() noexcept;
    int pending_steps_ = 0; unsigned frame_steps_ = 0; std::uint32_t step_epoch_ = 0; // UI: frame steps waiting, landed, in flight.
    void PumpSteps() noexcept;
    unsigned spatial_toggles_ = 0;
    HANDLE worker_ = nullptr;
    // Voice path. The voice thread owns capture, detector, gate and recogniser; recognised text crosses to the UI
    // thread in a one-cache-line record.
    // `decision`: an ai::WakeDecision (Listening carries no text).
    struct alignas(64) VoiceText { char text[116]; std::uint32_t bytes; float distance; std::uint32_t decision; }; // Hebrew takes 2 bytes a letter.
    static_assert(sizeof(VoiceText) == 128);
    core::SpscRingBuffer<VoiceText, 8>* voice_texts_ = nullptr;
    platform::windows::Win32WasapiCapture capture_;
    audio::MicroVad voice_vad_;
    ai::AcousticIntentRecognizer recogniser_;
    ai::VoiceGate voice_gate_;
    ai::AvionWakeGate wake_gate_;
    ai::AvionWakeGate text_gate_; // Hear: a host's recognised text (UI thread), with the same policy as the voice's.
    HANDLE voice_thread_ = nullptr;
    std::atomic<int> voice_ready_{0};   // Voice thread: 1 prepared, -1 failed.
    std::atomic<bool> voice_stop_{false}, voice_script_done_{false};
    std::atomic<HRESULT> voice_error_{S_OK};
    std::atomic<std::uint64_t> voice_captured_{0};
    VoiceResult voice_[kMaxVoiceResults]{};
    unsigned voice_results_ = 0;
    std::int64_t frequency_ = 0;
    PlaybackOptions options_{};
    unsigned width_ = 0, height_ = 0, hidden_ = 0, retired_ = 0, seeks_ = 0, seek_results_ = 0;
    std::uint32_t epoch_ = 1; // UI-owned: the newest requested playback epoch.
    std::int64_t duration_ = 0;
    SeekResult results_[kMaxSeekResults]{};
    bool hevc_ = false, ten_bit_ = false, paused_ = false, seekable_ = false;
    std::atomic<std::uint32_t> worker_epoch_{1}; // Worker-published: epoch whose repositioning is complete.
    std::atomic<std::uint32_t> ready_epoch_{0};  // UI-published: newest epoch whose first picture was handed to the swap chain.
    std::atomic<std::int64_t> clock_started_{0}; // Worker-published: Now() when the endpoint first started (pre-roll over).
    std::atomic<std::uint32_t> preroll_pictures_{0};
    std::int64_t run_began_ = 0;
    Stalls stalls_{};
    std::atomic<std::int64_t> anchored_at_{0};   // Worker-published: Now() when the clock last re-anchored.
    std::atomic<std::int64_t> audio_start_{0};   // Worker-published: media time of the first PCM frame kept after a seek.
    std::atomic<unsigned> applied_seeks_{0}, superseded_seeks_{0}, continued_seeks_{0}, adopted_seeks_{0};
    // Queue seeks (docs/seeking.md): the worker publishes, before worker_epoch_, whether it adopted the epoch it applied,
    // and the epoch its decode chain started in (the last seek it applied that was not adopted).
    std::atomic<std::uint32_t> adopted_epoch_{0}, chain_epoch_{1};
    std::uint32_t adopt_wait_ = 0; // UI: a queue seek whose decision the renderer waits for (0: none).
    std::atomic<bool> queue_seek_{true};
    std::atomic<bool> continuation_{true};
    // Scrub previews: the worker writes, before worker_epoch_, each applied epoch's decision (slot epoch % 16: the epoch
    // when it was a preview, else 0), and once the preview picture has been submitted, that epoch in preview_held_
    // (nothing else of it follows: presentation may release it without successors in the reorder window).
    std::atomic<bool> scrub_preview_{true};
    std::atomic<std::uint32_t> preview_epochs_[16]{};
    std::atomic<std::uint32_t> preview_held_{0};
    std::atomic<unsigned> preview_seeks_{0};
    bool Preview(std::uint32_t epoch) const noexcept { return epoch && preview_epochs_[epoch % 16].load(std::memory_order_relaxed) == epoch; }
    // UI: a drag is under way (scrub requests since the last plain seek), its last request, and the preview state shown.
    bool scrubbing_ = false, showing_preview_ = false, preview_cued_ = false, scrub_forward_ = true;
    std::int64_t last_scrub_ = 0, scrub_request_ = 0; // The newest scrub request's time and target.
    std::int64_t scrub_from_ = 0; // Where playback was when the drag began (the jump a release makes, for "undo").
    unsigned settled_scrubs_ = 0;
    bool isolation_posted_ = false; // --test-isolation's tagged posts were made.
    SeekWork work_[kMaxSeekWork]{};              // Worker-written.
    std::atomic<unsigned> work_count_{0};
    std::uint32_t video_parser_ = 0;             // Parser index of the video track.
    std::atomic<bool> hold_controls_{false};     // Test hook: the worker leaves commands queued (to drain a burst at once).
    // Chapters and scenes (UI thread after Prepare).
    timeline::ChapterTimeline timeline_;
    timeline::SynthesisResult synthesis_{};
    std::int64_t timeline_prepare_ = 0;
    int chapter_catalogue_ = -1;   // The QuickTime chapter track's catalogue index (-1: none).
    unsigned cut_cursor_ = 0;      // Renderer().NextCut cursor.
    NavigationResult navigations_[kMaxNavigations]{};
    unsigned navigation_count_ = 0;
    bool chapter_script_ = false;
    bool WaitSeekable() noexcept;                      // Preparing: steps the scan until sample lookups work (5 s at most).
    void ImportChapters(const wchar_t* path) noexcept; // Preparing: the QuickTime chapter track's samples as chapters.
    void PrepareTimeline(const wchar_t* path) noexcept;
    void PackMarks() noexcept;                         // The dock's ticks from the timeline.
    // Scrub coalescing (UI thread).
    bool SendSeek(std::int64_t target, bool scrub = false) noexcept;
    std::int64_t SeekLimit(std::int64_t target) const noexcept; // Clamped to what a seek can land on.
    void PumpScrub() noexcept;
    std::int64_t scrub_target_ = 0, sent_at_ = 0; // sent_at_: Now() when the newest epoch was sent.
    unsigned coalesced_scrubs_ = 0;
    bool scrub_pending_ = false, in_flight_ = false;
    ScrubResult scrub_{};
    std::atomic<bool> ready_{false}, start_{false}, stop_{false}, stopped_{false}, teardown_{false}, input_done_{false};
    std::atomic<unsigned> error_{0}, audio_frames_{0}, validated_frames_{0}, discarded_{0}, skipped_{0}, discontinuities_{0};
    std::atomic<std::uint64_t> silence_frames_{0};
    std::atomic<unsigned> bridged_gaps_{0}, read_aheads_{0}; std::atomic<std::int64_t> least_lead_{-1};
    std::atomic<HRESULT> native_error_{S_OK};
    std::atomic<std::int64_t> audio_end_{0};
};
}
