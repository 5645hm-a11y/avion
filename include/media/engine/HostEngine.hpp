#pragma once
#include "media/ai/AcousticIntentRecognizer.hpp"
#include "media/ai/AvionWakeGate.hpp"
#include "media/ai/CustomWakeWord.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/engine/HostControl.hpp"
#include "media/ai/VoiceGate.hpp"
#include "media/audio/DspProcessor.hpp"
#include "media/audio/MicroVad.hpp"
#include "media/audio/PcmConverter.hpp"
#include "media/audio/PcmRingBuffer.hpp"
#include "media/audio/SoundLocator.hpp"
#include "media/audio/TimeStretcher.hpp"
#include "media/clock/PredictiveMasterClock.hpp"
#include "media/codec/H264PcmDecoder.hpp"
#include "media/codec/H264SyntaxParser.hpp"
#include "media/codec/HevcSyntaxParser.hpp"
#include "media/core/linear_memory_arena.hpp"
#include "media/core/spsc_ring_buffer.hpp"
#include "media/demux/Demuxer.hpp"
#include "media/engine/EngineModules.hpp"
#include "media/engine/FrameSignature.hpp"
#include "media/platform/FileBlockSource.hpp"
#include "media/subtitle/SubtitleSearch.hpp"
#include "media/subtitle/SubtitleTrack.hpp"
#include "media/subtitle/EmbeddedText.hpp"
#include <atomic>
#include "media/timeline/ChapterTimeline.hpp"
#include "media/ui/PictureAudioCures.hpp"
#include "media/video/EventCamera.hpp"
#include "media/video/HorizonLevel.hpp"
#include "media/video/Matte.hpp"
#include "media/video/Orientation.hpp"
#include "media/video/Perception.hpp"
#include "media/video/Stabilizer.hpp"
#include "media/video/TargetTracker.hpp"
#include "media/video/Vibrometry.hpp"
#include "media/video/VirtualImu.hpp"
#include <atomic>
#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Cache-line isolation pads the atomics on purpose.
#endif
namespace media::engine {
// What the host's decoders and presenter see (the C SDK's media_video_unit_t, media_audio_unit_t, media_picture_t).
struct HostVideoUnit {
    const std::byte* data; std::uint32_t bytes, nal_length_bytes;
    const std::byte* config; std::uint32_t config_bytes, codec;
    std::int64_t pts_100ns, dts_100ns, duration_100ns;
    std::uint32_t epoch, flags, width, height;
};
struct HostAudioUnit {
    const std::byte* data; std::uint32_t bytes;
    const std::byte* config; std::uint32_t config_bytes, codec, sample_rate, channels, flags;
    std::int64_t pts_100ns;
};
struct HostPicture {
    std::int64_t pts_100ns, duration_100ns;
    std::uint32_t epoch, sequence, width, height;
    const std::uint8_t* luma; const std::uint8_t* cb; const std::uint8_t* cr;
    std::uint32_t luma_stride, chroma_stride, flags;
    float view_zoom, view_x, view_y;
    const char* subtitle;
    const char* status;
    float view_roll;                                 // The view's turn (stabilised roll), radians, +: clockwise on screen.
    // API 6 (docs/sdk.md, "Rotation, the horizon and black bars"): how to show it: mirrored left to right (the file's tkhd), then
    // turned `quarter_turns` clockwise (the file's and the viewer's), then by the leveling turn `view_level` (radians, +:
    // clockwise on screen; the viewer's or auto-leveling's) zoomed in by `level_zoom` (>= 1: its corners stay covered).
    std::uint32_t quarter_turns = 0; bool mirrored = false;
    float view_level = 0, level_zoom = 1;
};
enum : std::uint32_t { kCodecH264 = 1, kCodecHevc = 2, kCodecAac = 4, kCodecOpus = 5, kCodecPcm = 7 };
enum : std::uint32_t { kUnitKey = 1, kUnitFlush = 2, kPictureDecoded = 1, kPictureHost = 2, kPictureLanding = 4, kPictureTracking = 8,
                       kPictureTexture = 16 };
// Hardware decoding in host mode (API 14; docs/sdk.md, "Hardware decoding in host mode"): a GPU the SDK gives the engine (on
// Windows the engine's own Direct3D 12 video decoder, its pictures converted on the GPU into texture output's shared textures:
// src/sdk/HostGpu.cpp). The engine goes on parsing (parameter sets, the DPB, picture order, what is shown when) and hands each
// picture as parsed to the GPU, its references named by the engine's picture slots (HostGpuSlot: a slot's generation and the GPU's
// handle of the picture in it); the GPU's pictures never come back to the CPU. Every call is on the host's thread (Step's).
struct HostGpuSlot { std::uint32_t generation = 0; platform::SurfaceHandle surface{}; bool valid = false; };
struct HostGpuShow { std::int64_t pts_100ns = 0; std::uint32_t epoch = 0; float view_zoom = 1, view_x = 0.5f, view_y = 0.5f; };
class HostGpu {
public:
    enum class Status : std::uint8_t { Submitted, Busy, Failed };
    // Open (Preparing, `arena` not sealed): the stream's coded size, crop, bit depth and reordering depth (pictures decoded ahead of
    // the one shown next). False: this GPU cannot decode it (no device, the profile or size refused, a deeper reordering than its
    // picture ring holds): the engine falls back to the host's decoder or its software bridge.
    virtual bool PrepareDecode(core::LinearMemoryArena& arena, const demux::TrackInfo& track, unsigned coded_width, unsigned coded_height,
                               unsigned crop_height, bool ten_bit, unsigned reorder) noexcept = 0;
    // Whether a picture can be decoded now (a surface and, for one to show, a picture slot of its own): asked before parsing.
    [[nodiscard]] virtual bool Ready() const noexcept = 0;
    // One picture, as parsed (exactly one of `h264`, `hevc`); `display`: it will be shown or dropped (Show or Discard), else it is
    // a reference only. `slots`: the engine's pictures by slot index. Submitted: `surface` names it from now on.
    virtual Status Decode(const codec::H264Picture* h264, const codec::HevcPicture* hevc, const core::MediaPacket& packet, const platform::BlockView& view,
                          bool display, const HostGpuSlot* slots, unsigned slot_count, platform::SurfaceHandle& surface) noexcept = 0;
    virtual void Unreference(platform::SurfaceHandle surface) noexcept = 0; // Out of the DPB.
    virtual void Discard(platform::SurfaceHandle surface) noexcept = 0;     // A picture to show, dropped or flushed: never shown.
    virtual bool Show(platform::SurfaceHandle surface, const HostGpuShow& show) noexcept = 0; // Drawn into a free texture, published.
    virtual bool Restart(std::uint32_t epoch) noexcept = 0; // A seek: the pictures of earlier epochs were all unreferenced or discarded.
    virtual void CloseDecode() noexcept = 0;                // Close: the GPU idle, the decoder's arena state forgotten.
protected:
    ~HostGpu() = default;
};
struct HostOptions {
    std::uint32_t modules = kAllModules;
    std::uint32_t audio_rate = 48000, audio_channels = 2; // What PumpAudio delivers.
    std::uint32_t maximum_seconds = 0;
    const char* subtitle_path = nullptr;                  // UTF-8.
    bool voice = false;                                   // FeedVoice: the host's microphone through the voice path.
    bool voice_wake = true, push_to_talk = false;
    const char* wake_word = nullptr;
    const ai::SpeechSource* wake_speech = nullptr;
    void (*present)(void* context, const HostPicture&) noexcept = nullptr; void* present_context = nullptr;
    int (*decode_video)(void* context, const HostVideoUnit&) noexcept = nullptr;
    std::int32_t (*decode_audio)(void* context, const HostAudioUnit&, float* out, std::uint32_t max_frames) noexcept = nullptr;
    void* decoder_context = nullptr;
    FrameCallback frame_callback = nullptr; void* frame_context = nullptr;
    HostGpu* gpu = nullptr;                               // API 14: hardware decoding (null: the host's decoder or the bridge).
};
enum class StepStatus : std::uint8_t { Playing, Ended, Failed };
// Live sensors (the C SDK's media_sensor_config_t, media_sensor_frame_t; docs/sdk.md, "Live sensors"): a camera's pictures
// and a microphone array's samples pushed by the host instead of a file.
enum : std::uint32_t { kPixelNv12 = 1, kPixelP010 = 2, kPixelRgba8 = 3, kSampleF32 = 1, kSampleS16 = 2 };
struct SensorOptions {
    std::uint32_t width = 0, height = 0;             // The camera (0 x 0: none): even, 64..4096 x 64..2304.
    std::uint32_t audio_rate = 0, audio_channels = 0; // The microphones (0 channels: none): 8000..192000 Hz, 1..8 channels.
    float field_of_view = 0;                         // Horizontal, radians (0: 90 degrees).
    float microphone_spacing = 0;                    // Between channels 0 and 1, metres (0: 0.2).
};
struct SensorFrame {
    std::uint32_t format = 0, width = 0, height = 0; // kPixel*.
    const std::uint8_t* planes[2]{};                 // NV12 / P010: luma, interleaved CbCr; RGBA8: the pixels.
    std::uint32_t strides[2]{};                      // Bytes per row of each plane.
    std::int64_t pts_100ns = 0;                      // The sensor's timestamp (increasing).
    video::EgoMotion ego{};                          // The camera's rotation since the previous frame (an IMU), when known.
};
// What the engine perceives now (the C SDK's media_frame_telemetry_t).
struct HostTelemetry {
    video::PerceptionResult perception{};
    bool horizon = false; float horizon_tilt = 0;    // API 6: the horizon's tilt (degrees, clockwise), once auto-leveling's estimate holds.
    bool bars = false; video::Matte::Rect active{};  // API 6: black bars encoded in the picture, and the picture inside them.
    audio::SoundEvent sound{}; bool sound_known = false;
    bool saccade = false; std::uint32_t saccades = 0;
    float view_zoom = 1, view_x = 0.5f, view_y = 0.5f, view_roll = 0;
    std::uint32_t target_id = 0;                     // API 7: the perceived object target tracking follows (0: none).
    video::VibrometryResult vibrometry{};            // API 9: remote vibrometry on the region below (none: not measured).
    video::AreaRect vibrometry_region{};
    bool vibrometry_following = false;               // API 10: the region follows the tracked target (its window goes on).
    bool virtual_imu = false;                        // API 10: the newest picture's camera motion is the virtual IMU's.
    video::VirtualImuResult virtual_motion{};        // API 10: what it measured (valid: the background agreed).
};
struct HostState {
    std::int64_t position_100ns = 0, duration_100ns = 0;
    std::uint32_t epoch = 0;
    bool paused = false, muted = false, saccade = false;
    std::uint32_t tracking = 0; // 0 off, 1 following, 2 lost (view held).
    float volume = 1, speed = 1, view_zoom = 1, view_x = 0.5f, view_y = 0.5f;
    std::uint32_t audio_track = 0, audio_tracks = 0;
    char audio_language[4]{};
    std::uint32_t presented = 0, dropped = 0, underruns = 0;
    std::uint32_t quarter_turns = 0; bool mirrored = false, auto_level = false, auto_crop = false; // API 6.
    float level_degrees = 0;
};

// Host-driven playback, on every platform (docs/sdk.md, "Host-driven playback"): the engine's portable pieces composed
// without a window, a GPU or an audio endpoint, for the C SDK's MEDIA_PLAYBACK_HOST.
//
//   file -> FileBlockSource -> IsobmffParser -> video: H264SyntaxParser / HevcSyntaxParser (DPB, POC) -> H264PcmDecoder or
//                                                      the host's decoder -> pictures in presentation order -> Step presents
//                                               audio: PCM (or the host's decoder) -> PcmConverter -> PcmRingBuffer
//   PumpAudio (the host's audio thread): ring -> DspProcessor (EQ, notch killer, audio zoom, night mode, clarity)
//                                        -> TimeStretcher (speed) -> OutputGain (volume, mute) -> the host; the clock
//                                        (PredictiveMasterClock) follows what was pumped and when.
//   FeedVoice: MicroVad -> VoiceGate -> AcousticIntentRecognizer -> AvionWakeGate -> (ring) -> Step: LocalIntentDispatcher.
//
// The state machine is the engine's: Closed -> Preparing (Open: arena, tables, templates) -> Primed (sealed) -> Running
// (Step) -> Draining (a seek, a track switch: queues flushed, pools kept) -> Running ... -> Stopped (the end). Nothing is
// allocated after Open; no thread is started; Step and PumpAudio exchange only through the SPSC ring and atomics.
class HostEngine final {
public:
    HostEngine() noexcept;
    HostEngine(const HostEngine&) = delete;
    HostEngine& operator=(const HostEngine&) = delete;
    ~HostEngine() noexcept { Close(); }
    // The core of the arena in host mode (file blocks, demux index, decoded pictures, the audio path; measured, docs/sdk.md)
    // and the modules that mean something without a window (voice, subtitles, chapters, telemetry: their usual regions).
    static constexpr std::uint64_t kCoreArenaBytes = 48ull << 20;
    static constexpr std::uint32_t kHostModules = kModuleVoice | kModuleSubtitles | kModuleChapters | kModuleTelemetry;
    // API 14: with the GPU decoding (HostOptions::gpu), the file's blocks are 4 MiB (FileBlockSource::kLargeBlockBytes): 24 MiB more.
    static constexpr std::uint64_t kGpuBlockBytes = std::uint64_t{platform::FileBlockSource::kSlots} *
                                                    (platform::FileBlockSource::kLargeBlockBytes - platform::FileBlockSource::kBlockBytes);
    [[nodiscard]] static constexpr std::uint64_t ArenaBudget(std::uint32_t modules, bool gpu = false) noexcept {
        return kCoreArenaBytes + engine::ArenaBudget(modules & kHostModules) - engine::kCoreArenaBytes + (gpu ? kGpuBlockBytes : 0);
    }
    // Preparing -> Primed. False with Error() (a stage number) on failure; the engine is then Closed.
    [[nodiscard]] bool Open(const char* utf8_path, const HostOptions& options) noexcept;
    // Live sensors instead of a file (docs/sdk.md, "Live sensors"): Preparing -> Primed with no file, no demuxer and no decoder.
    // Each IngestVideo is shown at once (the tracker, perception, the saccade, the signature, the host's on_present);
    // IngestAudio feeds the sound locator (the raw pair) and, when the host pumps audio, the ring PumpAudio plays through the
    // DSP (EQ, notch killer, the audio zoom's beam). One thread for video (and Step, commands), one for audio.
    [[nodiscard]] bool OpenSensor(const SensorOptions& sensor, const HostOptions& options) noexcept;
    bool IngestVideo(const SensorFrame& frame) noexcept;
    bool IngestAudio(const void* samples, std::uint32_t format, std::uint32_t frames, std::int64_t pts_100ns) noexcept;
    [[nodiscard]] std::uint32_t AudioOverflow() const noexcept { return audio_overflow_.load(std::memory_order_relaxed); } // Frames the ring had no room for.
    // Perception and the audio saccade (docs/picture.md, "Perception"; docs/audio-dsp.md, "Sound localisation").
    [[nodiscard]] HostTelemetry Telemetry() const noexcept;
    [[nodiscard]] const video::Perception& Perceiver() const noexcept { return perception_; }
    ai::IntentStatus SetSaccade(bool on) noexcept;
    // Several objects at once (docs/picture.md, "Several objects at once", API 7): target tracking moves onto one of the objects
    // perception keeps track of (video::PerceivedObject): `track_id`, or 0 for the next after the one followed now in the
    // table's order (the most urgent first; the first when none is followed: "next target" starts tracking). The view goes to
    // it at once (and the audio focus's beam with it), the tracker locks onto it on the next picture. NotFound: no such
    // object; NoContext: the one followed is the only one.
    ai::IntentStatus TrackObject(std::uint32_t track_id) noexcept;
    [[nodiscard]] std::uint32_t TargetId() const noexcept { return target_id_; }
    // Electronic image stabilisation (docs/picture.md, "Stabilisation"): the view handed to the host (HostPicture, telemetry)
    // moves with the shake inside a kZoom crop. The shake: a live sensor's IMU, else (2026-11-03) the virtual IMU's (the
    // pictures the engine holds without one: a webcam, the software bridge's files; it needs telemetry). Unavailable when the
    // host's decoder holds the pictures.
    ai::IntentStatus SetStabilization(bool on) noexcept;
    [[nodiscard]] bool Stabilizing() const noexcept { return stabilize_; }
    // Rotation, the horizon and black bars (docs/sdk.md, "Rotation, the horizon and black bars", API 6): the host draws the
    // picture, so the engine says how (HostPicture: the turns, the mirror, the leveling turn and its zoom). The pictures the
    // engine holds (its software bridge, live sensors) are surveyed as the windowed engine's statistics pass surveys its own:
    // video::HorizonLevel's histograms (the horizon's tilt, which auto-leveling follows) and video::Matte's extents (black bars
    // encoded in the picture, which auto-crop fills the view without), when telemetry is on or either is. The viewer's turns:
    // SetOrientation (quarter turns clockwise, a leveling turn within the renderer's 15 degrees; auto-leveling stops), and the
    // commands ("rotate video", "level horizon", "crop black bars"). A new file starts upright as its tkhd says.
    ai::IntentStatus SetOrientation(unsigned quarter_turns, float level_degrees) noexcept;
    ai::IntentStatus SetAutoLevel(bool on) noexcept;
    ai::IntentStatus SetAutoCrop(bool on) noexcept;
    [[nodiscard]] const video::Orientation& OrientationNow() const noexcept { return orientation_; }
    [[nodiscard]] const video::HorizonLevel& Horizon() const noexcept { return horizon_; }
    [[nodiscard]] const video::Matte& Bars() const noexcept { return matte_; }
    [[nodiscard]] std::uint32_t Surveyed() const noexcept { return surveyed_; } // Pictures surveyed (horizon and bars).
    // The view shown: the engine's (zoom, saccade, tracking), stabilised when on (with its roll: the view's turn, radians,
    // +: clockwise on screen; 0 unstabilised).
    void ShownView(float& zoom, float& view_x, float& view_y) const noexcept { float roll = 0; ShownView(zoom, view_x, view_y, roll); }
    void ShownView(float& zoom, float& view_x, float& view_y, float& roll) const noexcept;
    // How the newest picture's scene moved since the previous one, from its IMU deltas or the virtual IMU's (2026-11-03): (dx,
    // dy) pixels at the centre and a turn about it (radians, picture coordinates). Zeros when neither measured it.
    void SceneMotion(float& dx, float& dy, float& roll) const noexcept;
    // The newest ingested camera picture, as converted (8-bit 4:2:0, three planes): for the GPU path (docs/sdk.md, "GPU live
    // ingest"). False before the first. Valid until the next IngestVideo but one.
    bool SensorPicture(const std::uint8_t*& luma, const std::uint8_t*& cb, const std::uint8_t*& cr, std::uint32_t& luma_stride,
                       std::uint32_t& chroma_stride, std::int64_t& pts_100ns) const noexcept;
    // Forensic snapshots (docs/picture.md, "Forensic snapshots", 2026-11-01): the picture shown last as the engine holds it (its
    // software bridge's or a camera's: the crop's 8-bit 4:2:0 planes). False before the first, or when the host's decoder holds
    // it. Valid until the next Step or IngestVideo. "take snapshot" (and the other languages) asks the SDK for one:
    // TakeSnapshotRequest says so once.
    bool ShownPicture(const std::uint8_t*& luma, const std::uint8_t*& cb, const std::uint8_t*& cr, std::uint32_t& luma_stride, std::uint32_t& chroma_stride,
                      std::uint32_t& width, std::uint32_t& height, std::int64_t& pts_100ns) const noexcept;
    bool TakeSnapshotRequest() noexcept { const bool asked = snapshot_requested_; snapshot_requested_ = false; return asked; }
    // Remote vibrometry (docs/picture.md, "Remote vibrometry", 2026-11-02): with telemetry, every picture the engine holds (its
    // software bridge's, a camera's) is sampled over a region held still in the scene: while tracking, a region of the fovea's
    // size centred on the target's path (video::TargetPath; since 2026-11-03 it follows the target and the window goes on:
    // "Moving targets"; a cut, a lost track or a lock on another target starts it over), else the view's fovea (kFovea of the
    // view across and down, at its centre) moved with the camera's shake (the IMU's or the virtual IMU's motion less its part
    // slower than kSteadySeconds). video::Vibrometry finds the strongest vibration and a pulse in it (HostTelemetry). Null:
    // not measured (no telemetry, no picture).
    static constexpr float kFovea = 0.5f;
    static constexpr double kSteadySeconds = 3.0;
    [[nodiscard]] const video::Vibrometry* Vibrometer() const noexcept { return vibro_; }
    // The virtual IMU (docs/picture.md, "The virtual IMU", 2026-11-03): the pictures the engine holds without IMU deltas (a
    // webcam, the software bridge's files) give the camera's motion from the background's (video::VirtualImu): stabilisation,
    // vibrometry's fovea, the event camera and perception take it as an IMU's. Configured with telemetry.
    [[nodiscard]] const video::VirtualImu& VirtualMotion() const noexcept { return virtual_imu_; }
    // The event camera (docs/picture.md, "Event camera", 2026-11-02): the pictures the engine holds as a neuromorphic
    // sensor's events (video::EventCamera), stabilised by the camera's IMU motion; off until asked for (null: off).
    // Unavailable without such pictures or without telemetry (its sensor is made at open, ~3.5 MiB at most).
    ai::IntentStatus SetEventCamera(const video::EventCameraOptions* options) noexcept;
    [[nodiscard]] bool EventCameraOn() const noexcept { return events_on_; }
    [[nodiscard]] video::EventCamera& Events() noexcept { return events_; }
    [[nodiscard]] const video::EventCamera& Events() const noexcept { return events_; }
    // Tensor export (docs/picture.md, "Tensor export", 2026-11-02): the region of the picture shown last (ShownPicture's
    // samples) a tensor of `aspect` (width / height) is taken from. Fovea: the tracked object's box (or the tracker's window)
    // grown by a fifth while tracking, else the view's fovea; its shape made the tensor's (the smaller side grown) and kept
    // inside the picture. View: the view shown (zoom, pan, stabilisation). Full: the picture. Custom: fractions of it.
    enum class TensorRegion : std::uint8_t { Fovea, View, Full, Custom };
    bool TensorArea(TensorRegion kind, video::AreaRect fractions, double aspect, video::AreaRect& out) const noexcept;
    [[nodiscard]] bool AutoLevel() const noexcept { return auto_level_; }
    void Close() noexcept;
    [[nodiscard]] StepStatus Step(std::int64_t now_100ns) noexcept;
    // One consumer thread. Writes `frames` interleaved frames; returns how many were programme (the rest silence).
    std::uint32_t PumpAudio(float* out, std::uint32_t frames, std::int64_t now_100ns) noexcept;
    bool FeedVoice(const float* samples, std::uint32_t frames) noexcept;
    ai::Intent Command(std::string_view utf8) noexcept;
    // API 15: a structured control (HostControl.hpp), as the intent its command would have become. Frame steps: Unavailable.
    ai::IntentStatus Control(HostControl op, double value) noexcept;
    bool Hear(std::string_view utf8) noexcept;
    void SetTalk(bool held) noexcept { text_gate_.SetTalk(held); wake_gate_.SetTalk(held); }
    bool SetWake(bool required, bool push_to_talk, const char* word) noexcept;
    bool PollFrame(FrameSignature& out) noexcept;
    [[nodiscard]] HostState State() const noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return open_; }
    [[nodiscard]] std::uint32_t Error() const noexcept { return error_; }
    [[nodiscard]] core::ArenaStatistics ArenaStats() const noexcept { return arena_.Statistics(); }
    [[nodiscard]] std::uint64_t ModuleArenaBytes() const noexcept { return module_bytes_; }
    [[nodiscard]] std::uint32_t Presented() const noexcept { return presented_; }
    [[nodiscard]] std::uint32_t Dropped() const noexcept { return dropped_; }
    [[nodiscard]] std::uint32_t Signatures() const noexcept { return signature_count_; }
    [[nodiscard]] std::uint32_t SignaturesLost() const noexcept { return signatures_lost_; }
    [[nodiscard]] std::uint32_t WakeIgnored() const noexcept { return text_gate_.Stats().ignored + wake_gate_.Stats().ignored; }
    [[nodiscard]] unsigned WakeTemplates() const noexcept { return wake_templates_; }
    [[nodiscard]] std::uint32_t Undecodable() const noexcept { return undecodable_; } // Pictures neither decoder could make.
    // Who decodes the video (API 14, media_query_video_decoder): none (no video), the software bridge, the host's decoder, the GPU.
    enum class Decoder : std::uint8_t { None, Software, Host, Hardware };
    [[nodiscard]] Decoder VideoDecoder() const noexcept {
        return video_index_ < 0 || sensor_ ? Decoder::None : gpu_ ? Decoder::Hardware : bridge_ ? Decoder::Software : Decoder::Host;
    }
    enum class Phase : std::uint8_t { Closed, Preparing, Primed, Running, Draining, Stopped };
    [[nodiscard]] Phase CurrentPhase() const noexcept { return phase_; }
    // ---- Tracks (API 17, media_engine_track_count/_info in host mode; docs/sdk-reference.md, "Tracks") ----
    // Every track of the file (refused ones too) and the subtitle sidecar (id 0, when one is open), fixed from open to close: any
    // thread may read them. Audio tracks are the ones this engine plays (PCM, or the host's decoder); subtitle tracks are the
    // file's own (MP4 timed text, Matroska text and ASS) and the sidecar, each a channel of the subtitle track.
    struct CatalogueEntry {
        std::uint32_t id = 0; demux::TrackKind kind = demux::TrackKind::Other; demux::Codec codec = demux::Codec::Unknown;
        char language[4]{}; char title[32]{};
        std::uint32_t channels = 0, rate = 0, width = 0, height = 0;
        bool selectable = false, is_default = false, video = false;
        std::uint8_t audio_slot = 0xff;   // Audio: its slot among tracks_.
        std::int8_t channel = -1;         // Subtitles: its channel of the subtitle track.
    };
    static constexpr unsigned kMaxCatalogue = 17;
    [[nodiscard]] unsigned CatalogueCount() const noexcept { return catalogue_count_; }
    [[nodiscard]] const CatalogueEntry& CatalogueAt(unsigned i) const noexcept { return catalogue_[i < kMaxCatalogue ? i : 0]; }
    // The audio track playing and the subtitle track shown (an id; -1 none), as the host's thread last set them.
    [[nodiscard]] std::uint32_t ActiveAudio() const noexcept { return active_audio_.load(std::memory_order_acquire); }
    [[nodiscard]] std::int64_t ActiveSubtitles() const noexcept { return active_subtitles_.load(std::memory_order_acquire); }
    // Subtitles after open (API 17; the host's thread): a .srt/.vtt file (UTF-8 path, at most kMaxAttachBytes) becomes the sidecar
    // (id 0) and is shown, the file's own subtitle tracks kept. Parsed into the subtitle region's other half (reserved at open,
    // nothing allocated) and swapped in at once; the previous text stays valid until the next attach. False: no subtitles
    // module, an unreadable or too large file, or no cue in it (the subtitles shown are unchanged).
    static constexpr std::uint32_t kMaxAttachBytes = 4u << 20;
    [[nodiscard]] bool AttachSubtitles(const char* utf8_path) noexcept;
private:
    static constexpr unsigned kPictures = 12, kMaxAudioTracks = 8, kRingFrames = 32768, kChunk = 1024, kAheadMs = 250;
    struct Picture {
        std::uint8_t* y = nullptr; std::uint8_t* cb = nullptr; std::uint8_t* cr = nullptr;
        std::int64_t pts = 0, duration = 0;
        std::uint32_t epoch = 0, generation = 0;
        bool queued = false;     // Decoded, waiting to be presented (or dropped).
        bool referenced = false; // In the DPB.
        bool decoded = false;    // Planes hold it (the software bridge).
        // Hardware decoding (API 14): the GPU's picture, to show or discard (gpu_display) and in the GPU's DPB (gpu_reference).
        platform::SurfaceHandle gpu{}; bool gpu_display = false, gpu_reference = false;
    };
    struct AudioTrack { std::uint32_t index = 0, id = 0, rate = 0, channels = 0; demux::Codec codec{}; audio::PcmConverter* converter = nullptr; char language[4]{}; };
    struct VoiceText { char text[124]; std::uint32_t bytes; };
    static_assert(sizeof(VoiceText) == 128);
    bool Fail(std::uint32_t stage) noexcept { error_ = stage; return false; }
    bool PrepareModules(const HostOptions&) noexcept;
    bool PrepareVideo() noexcept;
    bool PrepareAudio() noexcept;
    bool PrepareSubtitles(const char* path) noexcept;
    void Catalogue() noexcept;                  // catalogue_ from the demuxer's tracks (after the subtitles, the audio slots).
    bool ImportEmbedded() noexcept;             // The file's subtitle tracks as WebVTT texts in embedded_arena_ (local file reads).
    bool BuildSubtitles(unsigned buffer, std::string_view sidecar) noexcept; // Track and search index `buffer` from the sidecar and embedded texts.
    [[nodiscard]] subtitle::SubtitleTrack& Subs() noexcept { return subtitle_tracks_[live_subtitles_]; }
    [[nodiscard]] const subtitle::SubtitleTrack& Subs() const noexcept { return subtitle_tracks_[live_subtitles_]; }
    [[nodiscard]] subtitle::SearchIndex& Search() noexcept { return searches_[live_subtitles_]; }
    [[nodiscard]] const subtitle::SearchIndex& Search() const noexcept { return searches_[live_subtitles_]; }
    void PublishSubtitles() noexcept;           // active_subtitles_ from what is shown.
    bool PrepareVoice(const HostOptions&) noexcept;
    // Step's parts.
    bool Pull() noexcept;                       // Packets in, while the pools have room.
    bool TakePacket(const core::MediaPacket&, bool& consumed) noexcept;
    bool DecodeVideo(const core::MediaPacket&, std::span<const std::byte> bytes, const platform::BlockView& view, bool& consumed) noexcept;
    void GpuForget(Picture&, bool display, bool reference) noexcept; // Hardware decoding: what the slot no longer needs of the GPU.
    bool DecodeAudio(const core::MediaPacket&, std::span<const std::byte> bytes, bool& consumed) noexcept;
    bool WritePcm(const float* frames, std::uint32_t count, unsigned channels) noexcept;
    void Present(std::int64_t now) noexcept;
    void Show(Picture&, std::uint32_t flags, std::int64_t now) noexcept;
    void Signature(const Picture&, std::int64_t now) noexcept;
    void FollowTarget(const Picture&) noexcept;
    void Perceive(const Picture&) noexcept;            // Perception on the picture shown.
    void FollowObject() noexcept;                      // Which perceived object the tracker is on (target_id_).
    // The perceived object holding (x, y) (its box and a twentieth of the picture around it), the target's own track first; 0: none.
    [[nodiscard]] std::uint32_t ObjectAt(float x, float y) const noexcept;
    void Survey(const Picture&) noexcept;              // Its horizon and black bars (auto-leveling, auto-crop, telemetry).
    void Sense(const Picture&) noexcept;               // The camera's motion to it (IMU or virtual), the stabiliser, the steady fovea.
    void Vibrate(const Picture&) noexcept;             // Remote vibrometry on it.
    void Spike(const Picture&) noexcept;               // The event camera on it.
    [[nodiscard]] video::AreaRect FoveaArea() const noexcept; // The view's fovea, steadied (samples of the crop).
    void PrepareSensing(unsigned width, unsigned height) noexcept; // Vibrometry and the event camera's sensor (telemetry).
    void FollowBars() noexcept;                        // The view to the picture inside the bars (auto-crop).
    void Saccade(std::int64_t pts) noexcept;           // The view toward a new sound, and its slew.
    bool PrepareOutput() noexcept;                     // The pumped audio path (ring, DSP, speed, gain).
    bool FinishOpen(const HostOptions&) noexcept;      // Gates, voice, telemetry, commands; sealed (Primed).
    void DrainVoice() noexcept;
    [[nodiscard]] std::int64_t MediaNow(std::int64_t now) const noexcept;
    bool Seek(std::int64_t target) noexcept;
    bool SelectAudio(std::uint32_t slot) noexcept;
    bool Pause(bool pause) noexcept;
    void FreePicture(Picture&) noexcept;
    [[nodiscard]] Picture* FreeSlot() noexcept;
    [[nodiscard]] ai::MediaBounds Bounds() const noexcept;
    static ai::IntentStatus HandleIntent(void* context, const ai::Intent&) noexcept;
    ai::IntentStatus Execute(const ai::Intent&) noexcept;
    static void HandleWake(void* context, ai::WakeDecision, std::string_view text, const audio::Utterance&) noexcept;
    ai::IntentStatus Cure(ai::IntentKind kind, bool undo) noexcept;
    ai::IntentStatus Track(bool on) noexcept;
    void SyncFocus() noexcept;

    // Setup.
    HostOptions options_{};
    core::LinearMemoryArena arena_;
    core::LinearMemoryArena voice_arena_, subtitle_arena_, chapter_arena_, telemetry_arena_;
    std::uint64_t module_bytes_ = 0;
    platform::FileBlockSource source_;
    demux::Demuxer parser_; // ISO BMFF or Matroska (API 17).
    core::SpscRingBuffer<core::MediaPacket, 64>* packets_ = nullptr;
    core::MediaPacket held_{}; bool have_held_ = false;
    Phase phase_ = Phase::Closed;
    bool open_ = false, parsed_ = false;
    std::uint32_t error_ = 0;
    // Video.
    std::int32_t video_index_ = -1; std::uint32_t video_id_ = 0; demux::TrackInfo video_{};
    std::byte* video_config_ = nullptr; std::uint32_t video_config_bytes_ = 0;
    codec::H264SyntaxParser h264_; codec::HevcSyntaxParser hevc_;
    bool bridge_ = false;              // The software bridge decodes (no host decoder, H.264).
    bool gpu_ = false;                 // API 14: the GPU decodes (HostOptions::gpu accepted the stream).
    Picture pictures_[kPictures]{};
    int shown_ = -1;                   // The slot shown last (ShownPicture), -1: none.
    bool snapshot_requested_ = false;  // A snapshot command not yet taken by the SDK.
    std::uint32_t coded_width_ = 0, coded_height_ = 0, crop_left_ = 0, crop_top_ = 0, crop_width_ = 0, crop_height_ = 0;
    std::uint32_t generation_ = 0, undecodable_ = 0;
    bool need_key_ = false, flush_unit_ = true;
    // Audio (Step's side).
    AudioTrack tracks_[kMaxAudioTracks]{}; unsigned track_count_ = 0, track_ = 0; bool audio_ = false;
    std::byte* audio_config_[kMaxAudioTracks]{}; std::uint32_t audio_config_bytes_[kMaxAudioTracks]{};
    float* decoded_ = nullptr; float* converted_ = nullptr; // kChunk * 8 frames each (the track's channels, then the output's).
    std::uint32_t decoded_capacity_ = 0;
    std::int64_t audio_trim_ = INT64_MIN; // The epoch's start: audio before it is dropped.
    bool audio_flush_unit_ = true;
    std::int64_t written_end_ = 0;        // Media time just after the last frame written to the ring.
    audio::PcmRingBuffer ring_;
    // Audio (PumpAudio's side).
    audio::DspProcessor dsp_;
    audio::SpectralTracker* analysis_ = nullptr;
    audio::OutputGain gain_;
    audio::TimeStretcher stretch_;
    float* pump_ = nullptr;               // kChunk frames of the output's channels.
    clock::PredictiveMasterClock clock_;
    std::uint64_t pumped_ = 0;            // Frames given to the host since open (silence included).
    std::uint64_t stretch_base_ = 0;      // Stretcher output frames at this epoch's start.
    std::uint32_t pump_epoch_ = 0, generation_clock_ = 0;
    bool anchored_ = false; float anchored_speed_ = 1;
    // Shared: Step writes, PumpAudio reads (or the reverse for the acknowledgements), each on its own cache line.
    audio::DspControls controls_;
    alignas(64) std::atomic<std::uint32_t> flush_request_{0};
    alignas(64) std::atomic<std::uint32_t> flush_ack_{0};
    alignas(64) std::atomic<std::int64_t> ring_origin_{0};
    alignas(64) std::atomic<bool> paused_{false};
    alignas(64) std::atomic<float> speed_{1.0f};
    alignas(64) std::atomic<std::uint32_t> underruns_{0};
    alignas(64) std::atomic<float> speech_now_{-1.0f};
    std::atomic<std::uint32_t> band_now_{audio::SpectralTracker::kBands};
    std::atomic<float> band_db_now_{-144.0f};
    // Playback.
    std::uint32_t epoch_ = 1;
    std::int64_t duration_ = 0, landing_ = 0, paused_at_ = 0, end_ = 0;
    bool landing_due_ = true;           // The next picture of the epoch is shown at once (start, seek).
    std::int64_t wall_media_ = 0, wall_now_ = INT64_MIN; // The clock without audio.
    std::int64_t last_now_ = 0;
    std::uint32_t presented_ = 0, dropped_ = 0, sequence_ = 0;
    // Subtitles, chapters.
    // API 17: two subtitle states (track and search index), the live one and the one an attach builds; the subtitle module's region
    // split into the sidecar's read buffer, the embedded tracks' texts, and one region per state.
    subtitle::SubtitleTrack subtitle_tracks_[2]; subtitle::SearchIndex searches_[2]; unsigned live_subtitles_ = 0;
    core::LinearMemoryArena read_arena_, embedded_arena_, state_arenas_[2];
    std::string_view embedded_[subtitle::SubtitleTrack::kMaxChannels]{}; std::uint32_t embedded_ids_[subtitle::SubtitleTrack::kMaxChannels]{};
    unsigned embedded_count_ = 0; bool sidecar_ = false;
    CatalogueEntry catalogue_[kMaxCatalogue]{}; unsigned catalogue_count_ = 0;
    std::atomic<std::uint32_t> active_audio_{0}; std::atomic<std::int64_t> active_subtitles_{-1};
    char caption_[512]{};
    bool subtitles_shown_ = true;
    // Commands.
    ai::LocalIntentDispatcher dispatcher_; ai::ConversationState conversation_; ui::PictureAudioCures cures_;
    // Voice.
    audio::MicroVad vad_; ai::AcousticIntentRecognizer recogniser_; ai::VoiceGate voice_gate_; ai::AvionWakeGate wake_gate_, text_gate_;
    core::SpscRingBuffer<VoiceText, 8>* voice_texts_ = nullptr;
    unsigned wake_templates_ = 0; bool voice_on_ = false;
    std::uint64_t voice_frames_ = 0;
    // Telemetry.
    core::SpscRingBuffer<FrameSignature, 64>* signatures_ = nullptr;
    std::uint16_t* codes_ = nullptr; std::uint32_t code_count_ = 0, codes_epoch_ = 0; bool codes_valid_ = false; // Previous picture's block codes.
    timeline::CutDetector cuts_;
    std::uint32_t signature_count_ = 0, signatures_lost_ = 0;
    // The view and target tracking.
    float zoom_ = 1, view_x_ = 0.5f, view_y_ = 0.5f;
    video::TargetTracker tracker_;
    std::uint8_t* grid_ = nullptr; std::uint32_t grid_scale_ = 1, grid_width_ = 0, grid_height_ = 0;
    bool track_pending_ = false; // "track target": lock on the next picture shown.
    bool retarget_ = false; float retarget_x_ = 0.5f, retarget_y_ = 0.5f; // TrackObject: lock there instead of the view's centre.
    std::uint32_t target_id_ = 0; // The perceived object the tracker follows (API 7; 0: none).
    float grid_mean_ = -1;       // The previous picture's mean grid luma: a jump is a cut for the tracker at once.
    // Perception and the sound locator (file mode: on the programme in PumpAudio; sensors: on the microphones' pair).
    video::Perception perception_; std::uint32_t perception_epoch_ = 0; video::EgoMotion ego_{};
    video::EgoMotion frame_ego_{}; // The newest camera frame's IMU deltas (SceneMotion; ego_ is perception's, consumed).
    audio::SoundLocator locator_; bool locate_in_pump_ = false;
    bool saccade_ = false, saccade_moving_ = false; std::uint32_t saccade_seen_ = 0, saccades_ = 0;
    float saccade_x_ = 0.5f, fov_ = 1.5707963f; std::int64_t slew_pts_ = -1;
    video::Stabilizer stabilizer_; bool stabilize_ = false;
    // Remote vibrometry and the event camera (2026-11-02).
    video::Vibrometry* vibro_ = nullptr;
    double steady_x_ = 0, steady_y_ = 0, slow_x_ = 0, slow_y_ = 0; // The scene's motion (IMU, pixels) and its slow part.
    std::int64_t steady_pts_ = -1;
    video::TargetPath vibro_path_;                                 // The tracked target's path (samples of the crop).
    std::uint32_t locks_ = 0, vibro_lock_ = 0;                     // Tracker locks so far, and the one vibrometry follows.
    bool cut_now_ = false;                                         // The newest picture is a cut (the tracker's test).
    std::uint32_t vibro_epoch_ = 0, events_epoch_ = 0;
    // The virtual IMU (2026-11-03).
    video::VirtualImu virtual_imu_; std::uint32_t virtual_epoch_ = 0;
    bool imu_frame_ = false;                                       // The picture being shown came with IMU deltas.
    bool virtual_ego_ = false;                                     // frame_ego_ is the virtual IMU's.
    video::EventCamera events_; bool events_on_ = false;
    // Rotation, the horizon, black bars.
    video::Orientation orientation_{}; bool auto_level_ = false, auto_crop_ = false;
    video::HorizonLevel horizon_{}; video::Matte matte_{}; std::uint32_t cropped_changes_ = 0, surveyed_ = 0;
    // Live sensors.
    bool sensor_ = false; SensorOptions sensor_options_{};
    std::int64_t sensor_now_ = 0, sensor_last_ = -1; unsigned sensor_slot_ = 0;
    audio::PcmConverter* sensor_converter_ = nullptr; float* sensor_input_ = nullptr; // kChunk frames of the microphones' channels.
    std::atomic<std::uint32_t> audio_overflow_{0};
    // The status cue (host mode has no HUD: the host shows it), until the media time `status_until_`.
    char status_[96]{};
    std::int64_t status_until_ = 0;
    void Status(const char* utf8, std::int64_t seconds_100ns = 15000000) noexcept;
};
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
