// The C SDK (docs/sdk.md), portable: configuration, budgets, the wake word, the engine's lifecycle in host storage, and
// host-driven playback (media_engine_step, media_engine_pump_audio) on every platform. The windowed engine is
// CApiEngine.cpp's (Windows); elsewhere asking for it says MEDIA_ERROR_UNSUPPORTED.
#include "CApiEngine.hpp"
#include "ProbeText.hpp"
#include "media/engine/HostControl.hpp"
#include <cstdio>
#include "media/ai/CustomWakeWord.hpp"
#include "media/ai/VoiceLexicon.hpp"
#include "media/codec/BitstreamPerception.hpp"
#include "media/engine/EngineModules.hpp"
#include "media/platform/OutputFile.hpp"
#include "media/video/TensorExport.hpp"
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <new>
#include <string_view>
#include <type_traits>

namespace {
using namespace media::engine;
constexpr std::uint32_t Flag(media_module_flags f) noexcept { return static_cast<std::uint32_t>(f); }
static_assert(Flag(MEDIA_MODULE_VOICE) == kModuleVoice && Flag(MEDIA_MODULE_HUD) == kModuleHud && Flag(MEDIA_MODULE_SUBTITLES) == kModuleSubtitles &&
              Flag(MEDIA_MODULE_NEURAL_UPSCALE) == kModuleNeuralUpscale && Flag(MEDIA_MODULE_CHAPTERS) == kModuleChapters &&
              Flag(MEDIA_MODULE_TELEMETRY) == kModuleTelemetry && Flag(MEDIA_MODULE_ALL) == kAllModules, "the SDK's flags are the engine's");
static_assert(sizeof(media_frame_signature_t) == 64 && sizeof(media_engine_stats_t) == 64 && sizeof(media_module_budget_t) == 16);
static_assert(sizeof(FrameSignature) == sizeof(media_frame_signature_t) && offsetof(FrameSignature, scene_cut_pts_100ns) == offsetof(media_frame_signature_t, scene_cut_pts_100ns) &&
              offsetof(FrameSignature, dominant_band) == offsetof(media_frame_signature_t, dominant_band) &&
              offsetof(FrameSignature, luminance_mean) == offsetof(media_frame_signature_t, luminance_mean), "the C signature is the engine's");
static_assert(kFrameSceneCut == static_cast<std::uint32_t>(MEDIA_FRAME_SCENE_CUT) && kFrameMeasured == static_cast<std::uint32_t>(MEDIA_FRAME_MEASURED));
constexpr std::uint32_t U(unsigned v) noexcept { return v; }
static_assert(kCodecH264 == U(MEDIA_CODEC_H264) && kCodecHevc == U(MEDIA_CODEC_HEVC) && kCodecAac == U(MEDIA_CODEC_AAC) && kCodecOpus == U(MEDIA_CODEC_OPUS) &&
              kUnitKey == U(MEDIA_UNIT_KEY) && kUnitFlush == U(MEDIA_UNIT_FLUSH) && kPictureDecoded == U(MEDIA_PICTURE_DECODED) &&
              kPictureHost == U(MEDIA_PICTURE_HOST) && kPictureLanding == U(MEDIA_PICTURE_LANDING) && kPictureTracking == U(MEDIA_PICTURE_TRACKING));
bool WordValid(const char* word) noexcept {
    if (!word) return false;
    const auto n = std::strlen(word);
    if (!n || n > 31) return false;
    for (std::size_t i = 0; i < n; ++i) if (word[i] != ' ' && word[i] != '\t') return true;
    return false;
}
bool HostMode(const media_engine_config_t& c) noexcept { return c.playback == MEDIA_PLAYBACK_HOST; }
constexpr std::size_t kApi11ConfigBytes = offsetof(media_engine_config_t, parent_window);
bool ConfigSize(const media_engine_config_t* c) noexcept { return c && (c->struct_size == sizeof(*c) || c->struct_size == kApi11ConfigBytes); }
// The host's config as this version's: an older struct's fields copied, the newer ones zero.
media_engine_config_t Whole(const media_engine_config_t& c) noexcept {
    media_engine_config_t whole{}; std::memcpy(&whole, &c, (std::min<std::size_t>)(c.struct_size, sizeof(whole)));
    whole.struct_size = sizeof(whole);
    return whole;
}
// Embedding in host mode (API 12): an event for the host (counted when it has not polled), and the posted commands run by step.
void HostEmit(media_engine* e, const HostEvent& event) noexcept {
    auto* slot = e->host_events.AcquireWriteSlot();
    if (!slot) { ++e->host_events_lost; return; }
    *slot = event; slot->lost = e->host_events_lost; e->host_events_lost = 0;
    (void)e->host_events.CommitWriteSlot();
}
void HostDrain(media_engine* e) noexcept;
// Trampolines between the engine's noexcept callbacks and the host's C ones.
std::size_t Speak(void* context, const char* text, unsigned voice, int rate, float* out, std::size_t capacity) noexcept {
    const auto& c = static_cast<media_engine*>(context)->config;
    return c.speak ? c.speak(c.speak_context, text, voice, rate, out, capacity) : 0;
}
void Present(void* context, const HostPicture& p) noexcept {
    const auto& c = static_cast<media_engine*>(context)->config;
    const media_picture_t picture{p.pts_100ns, p.duration_100ns, p.epoch, p.sequence, p.width, p.height,
                                  reinterpret_cast<const uint8_t*>(p.luma), reinterpret_cast<const uint8_t*>(p.cb), reinterpret_cast<const uint8_t*>(p.cr),
                                  p.luma_stride, p.chroma_stride, p.flags, p.view_zoom, p.view_x, p.view_y, p.subtitle, p.status, p.view_roll,
                                  p.quarter_turns, p.mirrored ? 1 : 0, p.view_level, p.level_zoom};
    c.on_present(c.on_present_context, &picture);
}
int DecodeVideo(void* context, const HostVideoUnit& u) noexcept {
    const auto& c = static_cast<media_engine*>(context)->config;
    const media_video_unit_t unit{reinterpret_cast<const uint8_t*>(u.data), u.bytes, u.nal_length_bytes, reinterpret_cast<const uint8_t*>(u.config), u.config_bytes,
                                  u.codec, u.pts_100ns, u.dts_100ns, u.duration_100ns, u.epoch, u.flags, u.width, u.height};
    return c.decode_video(c.decoder_context, &unit);
}
std::int32_t DecodeAudio(void* context, const HostAudioUnit& u, float* out, std::uint32_t max_frames) noexcept {
    const auto& c = static_cast<media_engine*>(context)->config;
    const media_audio_unit_t unit{reinterpret_cast<const uint8_t*>(u.data), u.bytes, reinterpret_cast<const uint8_t*>(u.config), u.config_bytes,
                                  u.codec, u.sample_rate, u.channels, u.flags, u.pts_100ns};
    return c.decode_audio(c.decoder_context, &unit, out, max_frames);
}
void Frame(void* context, const FrameSignature& s) noexcept {
    const auto& c = static_cast<media_engine*>(context)->config;
    media_frame_signature_t signature; std::memcpy(&signature, &s, sizeof(signature));
    c.on_frame(c.on_frame_context, &signature);
}
HostOptions MakeOptions(media_engine* e) noexcept {
    const auto& c = e->config;
    HostOptions o;
    o.modules = c.modules; o.audio_rate = c.audio_rate ? c.audio_rate : 48000; o.audio_channels = c.audio_channels ? c.audio_channels : 2;
    o.maximum_seconds = c.maximum_seconds; o.subtitle_path = e->subtitles[0] ? e->subtitles : nullptr;
    o.voice = c.voice_input == MEDIA_VOICE_HOST;
    o.voice_wake = c.wake_policy != MEDIA_WAKE_DISABLED; o.push_to_talk = c.wake_policy == MEDIA_WAKE_DISABLED;
    o.wake_word = c.wake_policy == MEDIA_WAKE_CUSTOM ? e->wake_word : nullptr;
    if (c.speak) { e->speech = {&Speak, e, c.speak_voices}; o.wake_speech = &e->speech; }
#if defined(_WIN32)
    else if (o.voice && o.wake_word && e->system.Open()) { e->speech = e->system.Source(); o.wake_speech = &e->speech; }
#endif
    if (c.on_present) { o.present = &Present; o.present_context = e; }
    if (c.decode_video) o.decode_video = &DecodeVideo;
    if (c.decode_audio) o.decode_audio = &DecodeAudio;
    o.decoder_context = e;
    if (c.on_frame) { o.frame_callback = &Frame; o.frame_context = e; }
#if defined(_WIN32)
    // API 14: hardware decoding into texture output (the GPU first; open falls back when it cannot take the stream).
    if (c.output_flags & MEDIA_OUTPUT_TEXTURE) { e->host_gpu.Configure(c.output_width, c.output_height, media::sdk::OutputFormat(c.output_format)); o.gpu = &e->host_gpu; }
#endif
    return o;
}
media_status HostOpen(media_engine* e, const char* path, const media_sensor_config_t* sensor) noexcept {
    // A new engine object for every file (the parser, the decoders and the arena are set up once each).
    e->host_engine.~HostEngine(); ::new (&e->host_engine) HostEngine();
    e->media[0] = 0; e->snapshot = {}; e->snapshots = 0;
    if (path && std::strlen(path) < sizeof(e->media)) std::memcpy(e->media, path, std::strlen(path) + 1);
    const auto o = MakeOptions(e);
    e->ran = true; e->stage = 0; e->code = 0;
    bool opened = false;
    if (sensor) {
        SensorOptions s;
        s.width = sensor->width; s.height = sensor->height; s.audio_rate = sensor->audio_rate; s.audio_channels = sensor->audio_channels;
        s.field_of_view = sensor->field_of_view; s.microphone_spacing = sensor->microphone_spacing;
        opened = e->host_engine.OpenSensor(s, o);
    } else opened = e->host_engine.Open(path, o);
#if defined(_WIN32)
    e->system.Close(); // Its renditions are templates by now.
#endif
    if (!opened) { e->stage = e->host_engine.Error() ? e->host_engine.Error() : 1u; return MEDIA_ERROR_OPEN; }
    e->open = true;
    return MEDIA_OK;
}

// Forensic snapshots in host mode (API 8; docs/sdk.md, "Snapshots"), on the host's thread: the GPU image when the GPU path is
// on, else the software view of the picture the engine holds. The folder: the host's (its separator added) or the default.
void SnapshotFolder(const media_engine* e, char* out, std::size_t capacity) noexcept {
    out[0] = 0;
    const auto n = std::strlen(e->snapshot_folder);
    if (!n) { if (!media::platform::DefaultSnapshotFolder(out, capacity)) out[0] = 0; return; }
    if (n + 2 > capacity) return;
    std::memcpy(out, e->snapshot_folder, n + 1);
    if (out[n - 1] != '/' && out[n - 1] != '\\') {
#if defined(_WIN32)
        out[n] = '\\';
#else
        out[n] = '/';
#endif
        out[n + 1] = 0;
    }
}
// A path's file name without its extension ("sensor" for none).
void Stem(const char* path, char* out, std::size_t capacity) noexcept {
    const char* name = path && *path ? path : "sensor";
    for (const char* c = name; *c; ++c) if (*c == '/' || *c == '\\') name = c + 1;
    std::size_t n = std::strlen(name);
    for (std::size_t i = n; i-- > 0;) if (name[i] == '.') { n = i; break; }
    n = (std::min)(n, capacity - 1);
    std::memcpy(out, name, n); out[n] = 0;
}
media_status HostSnapshot(media_engine* e, const char* path, const media::video::SnapshotMemory* memory = nullptr) noexcept {
    using media::video::SnapshotEncoder;
    auto& h = e->host_engine;
    const auto t = h.Telemetry();
    media::video::SnapshotInfo info;
    info.media = e->media[0] ? e->media : nullptr; info.sequence = e->snapshots + 1;
    float roll = 0; h.ShownView(info.view_zoom, info.view_x, info.view_y, roll);
    constexpr float kDegrees = 57.29577951308232f;
    info.view_roll_degrees = roll * kDegrees;
    const auto& orientation = h.OrientationNow();
    info.quarter_turns = orientation.QuarterTurns(); info.mirrored = orientation.Mirrored(); info.level_degrees = orientation.level; info.auto_level = h.AutoLevel();
    info.horizon_known = t.horizon; info.horizon_degrees = t.horizon_tilt;
    info.target_id = t.target_id; info.object_count = t.perception.object_count;
    for (unsigned i = 0; i < info.object_count && i < media::video::kMaxPerceivedObjects; ++i) info.objects[i] = t.perception.objects[i];
    char folder[512], stem[256];
    SnapshotFolder(e, folder, sizeof(folder)); Stem(e->media, stem, sizeof(stem));
    media::video::SnapshotResult r;
    std::uint32_t source = MEDIA_SNAPSHOT_GPU_IMAGE;
    auto status = media::sdk::GpuSnapshot(e, path, folder, stem, info, r, memory);
    if (status == MEDIA_ERROR_UNSUPPORTED) {
        // The software view: the view's region of the picture held, turned and mirrored as shown; no scaling.
        media::video::ViewImage v; std::uint32_t width = 0, height = 0;
        if (!h.ShownPicture(v.luma, v.cb, v.cr, v.luma_stride, v.chroma_stride, width, height, info.pts_100ns))
            return e->config.decode_video || h.VideoDecoder() == HostEngine::Decoder::Hardware ? MEDIA_ERROR_UNSUPPORTED : MEDIA_ERROR_EMPTY;
        media::video::ViewRegion(width, height, info.view_zoom, info.view_x, info.view_y, info.region);
        v.left = info.region[0]; v.top = info.region[1]; v.right = info.region[2]; v.bottom = info.region[3];
        orientation.Display(v.display);
        info.source = "view"; source = MEDIA_SNAPSHOT_VIEW;
        status = media::video::SaveSnapshotTo(e->encoder, memory, path, folder, stem, v.Width(), v.Height(), &SnapshotEncoder::ViewRows, &v, info, r) ? MEDIA_OK :
                 memory ? MEDIA_ERROR_STORAGE : MEDIA_ERROR_OPEN;
    }
    if (memory) { e->last_memory_ = r; return status; } // API 17: the host's buffer; media_query_snapshot keeps the last file.
    if (status == MEDIA_ERROR_EMPTY) return status;
    auto& s = e->snapshot;
    s = {}; s.struct_size = sizeof(s); s.status = status; s.source = source; s.width = r.width; s.height = r.height; s.pts_100ns = r.pts_100ns;
    s.frame_hash = r.hash; std::memcpy(s.path, r.path, sizeof(s.path)); s.path[sizeof(s.path) - 1] = 0;
    s.sequence = status == MEDIA_OK ? ++e->snapshots : e->snapshots;
    return status;
}
// A command or utterance just run may have asked for one ("take snapshot"): written now. Its status, or MEDIA_OK when none was asked.
media_status PendingSnapshot(media_engine* e) noexcept { return e->host_engine.TakeSnapshotRequest() ? HostSnapshot(e, nullptr) : MEDIA_OK; }
void HostDrain(media_engine* e) noexcept {
    for (unsigned i = 0; i != 8; ++i) {
        const auto* c = e->host_commands.PeekReadSlot();
        if (!c) return;
        ++e->commands;
        const bool ok = c->op ? e->host_engine.Control(static_cast<media::engine::HostControl>(c->op), c->value) == media::ai::IntentStatus::Ok
                              : e->host_engine.Command(std::string_view{c->text, c->length}).status == media::ai::IntentStatus::Ok && PendingSnapshot(e) == MEDIA_OK;
        if (!ok) ++e->refused;
        HostEvent event{}; event.kind = HostEvent::kCommand; event.id = c->id; event.status = ok ? 0 : 1;
        (void)e->host_commands.ReleaseReadSlot();
        HostEmit(e, event);
    }
}
}

#if !defined(_WIN32)
namespace media::sdk {
media_status WindowOpen(media_engine*, const char*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowRun(media_engine*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
void WindowStop(media_engine*) noexcept {}
void WindowClose(media_engine*) noexcept {}
media_status GpuOpen(media_engine*, const media_sensor_config_t&) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
void GpuIngest(media_engine*, const media_sensor_frame_t&) noexcept {}
void GpuClose(media_engine*) noexcept {}
media_status GpuControls(media_engine*, const media_picture_controls_t&) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status GpuImage(media_engine*, media_gpu_image_t*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowCommand(media_engine*, const char*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowHear(media_engine*, const char*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
void WindowSetTalk(media_engine*, bool) noexcept {}
bool WindowSetWake(media_engine*, media_wake_policy) noexcept { return true; }
media_status WindowPoll(media_engine*, media_frame_signature_t*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
void WindowStats(const media_engine*, media_engine_stats_t*) noexcept {}
media_status WindowPostCommand(media_engine*, const char*, std::uint32_t) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowPostControl(media_engine*, std::uint32_t, double, std::uint32_t) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowTrackCount(const media_engine*, std::uint32_t&) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowTrackInfo(const media_engine*, std::uint32_t, media_track_info_t&) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowPollEvent(media_engine*, media::engine::HostEvent&) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowState(const media_engine*, media_playback_state_t*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowOutputTextures(media_engine*, media_output_textures_t*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowAcquireOutput(media_engine*, media_output_frame_t*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowReleaseOutput(media_engine*, std::uint32_t, std::uint64_t) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowResizeOutput(media_engine*, std::uint32_t, std::uint32_t) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
// API 14: no GPU decoding off Windows: the open fell back (HostOptions::gpu is null), so the texture calls have nothing to give.
media_status HostOutputTextures(media_engine*, media_output_textures_t*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status HostAcquireOutput(media_engine*, media_output_frame_t*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status HostReleaseOutput(media_engine*, std::uint32_t, std::uint64_t) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status HostResizeOutput(media_engine*, std::uint32_t, std::uint32_t) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status GpuSnapshot(media_engine*, const char*, const char*, const char*, media::video::SnapshotInfo&, media::video::SnapshotResult&,
                         const media::video::SnapshotMemory*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowSnapshotMemory(media_engine*, const media::video::SnapshotMemory&, media::video::SnapshotResult&) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowAttachSubtitles(media_engine*, const char*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowSnapshot(media_engine*, const char*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowSnapshotQuery(const media_engine*, media_snapshot_t*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowEventCamera(media_engine*, const media::video::EventCameraOptions*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowStabilization(media_engine*, bool) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowPollEvents(media_engine*, media::video::EventSpike*, std::uint32_t, std::uint32_t&) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowEventState(const media_engine*, media_event_camera_state_t*) noexcept { return MEDIA_ERROR_UNSUPPORTED; }
media_status WindowTensor(media_engine*, const media::video::TensorSpec&, const media_tensor_request_t&, void*, std::size_t, media_tensor_info_t*) noexcept {
    return MEDIA_ERROR_UNSUPPORTED;
}
void WindowSnapshotFolder(media_engine*) noexcept {}
}
#endif

using namespace media::sdk;
extern "C" {
void media_engine_config_init(media_engine_config_t* config) {
    if (!config) return;
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->modules = MEDIA_MODULE_ALL;
    config->wake_policy = MEDIA_WAKE_DEFAULT;
    config->voice_input = MEDIA_VOICE_NONE;
    config->visible = 1;
#if defined(_WIN32)
    config->playback = MEDIA_PLAYBACK_WINDOW;
#else
    config->playback = MEDIA_PLAYBACK_HOST;
#endif
    config->audio_rate = 48000; config->audio_channels = 2;
}
uint64_t media_query_arena_budget(const media_engine_config_t* config) {
    if (!ConfigSize(config) || (config->modules & ~static_cast<uint32_t>(MEDIA_MODULE_ALL))) return 0;
#if defined(_WIN32)
    const bool gpu = (config->struct_size == sizeof(*config) && (config->output_flags & MEDIA_OUTPUT_TEXTURE)); // API 14: host mode's GPU decoding.
#else
    const bool gpu = false;
#endif
    return HostMode(*config) ? HostEngine::ArenaBudget(config->modules, gpu) : ArenaBudget(config->modules);
}
media_status media_query_module_budget(uint32_t module, media_module_budget_t* budget) {
    const int index = ModuleIndex(module);
    if (index < 0 || !budget) return MEDIA_ERROR_INVALID_ARGUMENT;
    budget->arena_bytes = kModuleBudgets[index].arena_bytes; budget->threads = kModuleBudgets[index].threads; budget->reserved = 0;
    return MEDIA_OK;
}
int media_wake_word_hearable(const char* wake_word) {
    // Every word in the recogniser's vocabulary (wake, command and filler words), or a name it can synthesise (API 2).
    if (!WordValid(wake_word)) return 0;
    if (media::ai::CustomWakeWord::Readable(wake_word)) return 1;
    char folded[32]{}; std::size_t n = 0;
    for (const char* c = wake_word; *c && n < sizeof(folded) - 1; ++c) folded[n++] = static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    std::string_view rest{folded, n};
    int words = 0;
    while (!rest.empty()) {
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
        if (rest.empty()) break;
        const auto end = rest.find_first_of(" \t");
        const auto word = rest.substr(0, end);
        bool known = false;
        for (const auto& w : media::ai::voice_lexicon::kWords) known = known || (word == w.text && w.role != media::ai::voice_lexicon::Role::Other);
        if (!known) return 0;
        ++words;
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    }
    return words > 0;
}
size_t media_engine_storage_bytes(void) { return sizeof(media_engine); }
size_t media_engine_storage_alignment(void) { return alignof(media_engine); }
media_status media_engine_create(const media_engine_config_t* config, void* storage, size_t storage_bytes, media_engine_t** engine) {
    if (!ConfigSize(config) || !engine || !media_query_arena_budget(config)) return MEDIA_ERROR_INVALID_ARGUMENT;
    const auto whole = Whole(*config); // API 11's struct: no parent window.
    if (whole.parent_window && HostMode(whole)) return MEDIA_ERROR_INVALID_ARGUMENT; // Embedding is the windowed engine's.
    // Texture output (API 13): not inside a window, at a drawable size; API 14: host mode too (its GPU decoding), and a format.
    if (whole.output_flags & ~static_cast<uint32_t>(MEDIA_OUTPUT_TEXTURE)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if ((whole.output_flags & MEDIA_OUTPUT_TEXTURE) && (whole.parent_window || whole.output_width < 16 || whole.output_height < 16 ||
                                                       whole.output_width > 8192 || whole.output_height > 8192)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (whole.output_format > MEDIA_OUTPUT_FORMAT_SCRGB16 || (whole.output_format && !(whole.output_flags & MEDIA_OUTPUT_TEXTURE))) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (config->wake_policy < MEDIA_WAKE_DEFAULT || config->wake_policy > MEDIA_WAKE_CUSTOM) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (config->wake_policy == MEDIA_WAKE_CUSTOM && !WordValid(config->wake_word)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (config->voice_input < MEDIA_VOICE_NONE || config->voice_input > MEDIA_VOICE_HOST) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (config->playback != MEDIA_PLAYBACK_WINDOW && config->playback != MEDIA_PLAYBACK_HOST) return MEDIA_ERROR_INVALID_ARGUMENT;
    if ((config->voice_input == MEDIA_VOICE_HOST) != (HostMode(*config) && config->voice_input != MEDIA_VOICE_NONE)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (HostMode(*config) && (config->audio_channels > 2 || (config->audio_rate && (config->audio_rate < 8000 || config->audio_rate > 192000)))) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!storage || storage_bytes < sizeof(media_engine) || reinterpret_cast<std::uintptr_t>(storage) % alignof(media_engine)) return MEDIA_ERROR_STORAGE;
    const auto length = config->subtitle_path ? std::strlen(config->subtitle_path) : 0;
    if (length >= sizeof(media_engine::subtitles)) return MEDIA_ERROR_INVALID_ARGUMENT;
    auto* e = ::new (storage) media_engine();
    e->config = whole; e->host = HostMode(whole);
    std::memcpy(e->wake_word, config->wake_word, sizeof(e->wake_word)); e->wake_word[31] = 0;
    if (length) std::memcpy(e->subtitles, config->subtitle_path, length + 1);
    e->config.subtitle_path = nullptr; // Kept here (the host's string need not outlive create).
    *engine = e;
    return MEDIA_OK;
}
media_status media_engine_open(media_engine_t* e, const char* path) { return media_engine_open_ex(e, path, nullptr); }
void media_engine_cancel_open(media_engine_t* e) {
    if (e && e->opening.load(std::memory_order_acquire)) e->cancel_open.store(true, std::memory_order_release);
}
media_status media_engine_open_ex(media_engine_t* e, const char* path, const media_open_options_t* options) {
    if (!e || !path || (options && options->struct_size != sizeof(*options))) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (e->open) return MEDIA_ERROR_STATE;
    e->http_headers[0] = 0;
    if (options && options->http_headers && options->http_headers[0]) {
        const auto n = std::strlen(options->http_headers);
        constexpr std::size_t kWide = sizeof(e->http_headers) / sizeof(e->http_headers[0]);
        if (n >= kWide || !media::sdk::Utf8ToWide(options->http_headers, e->http_headers, kWide))
            return MEDIA_ERROR_INVALID_ARGUMENT;
    }
    e->open_failed = false; e->failed_reason[0] = 0;
    e->cancel_open.store(false, std::memory_order_relaxed); e->opening.store(true, std::memory_order_release);
    const auto status = e->host ? HostOpen(e, path, nullptr) : WindowOpen(e, path);
    e->opening.store(false, std::memory_order_release);
    const bool cancelled = e->cancel_open.exchange(false, std::memory_order_acq_rel) && status == MEDIA_ERROR_OPEN && e->code == static_cast<std::int32_t>(0x80004004u);
    if (cancelled) { e->open_failed = true; std::snprintf(e->failed_reason, sizeof(e->failed_reason), "cancelled (media_engine_cancel_open)"); return status; }
    if (status == MEDIA_ERROR_OPEN) {
        // API 15: why, in words (media_engine_last_error_text). A URL is not probed (no request is sent for a diagnosis).
        e->open_failed = true;
        const bool remote = std::strncmp(path, "http://", 7) == 0 || std::strncmp(path, "https://", 8) == 0;
        if (!remote) {
            auto* info = &e->probe_scratch; info->struct_size = sizeof(*info);
            if (media::sdk::Probe(path, *info) != MEDIA_ERROR_INVALID_ARGUMENT && info->reason[0])
                std::snprintf(e->failed_reason, sizeof(e->failed_reason), "%s", info->reason);
            else if (info->playable) std::snprintf(e->failed_reason, sizeof(e->failed_reason), "the container and codecs are supported; this machine's decoder or audio refused it");
            media::sdk::CutUtf8(e->failed_reason, sizeof(e->failed_reason));
        }
    }
    return status;
}
media_status media_engine_run(media_engine_t* e) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || e->host) return MEDIA_ERROR_STATE; // Host mode: media_engine_step.
    return WindowRun(e);
}
void media_engine_stop(media_engine_t* e) { if (e && !e->host) WindowStop(e); }
media_status media_engine_close(media_engine_t* e) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (e->host) { GpuClose(e); e->host_engine.Close(); } else WindowClose(e);
    e->open = false;
    return MEDIA_OK;
}
void media_engine_destroy(media_engine_t* e) {
    if (!e) return;
    if (e->open) (void)media_engine_close(e);
    e->~media_engine();
}
media_status media_engine_command(media_engine_t* e, const char* utf8) {
    if (!e || !utf8) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowCommand(e, utf8);
    ++e->commands;
    if (e->host_engine.Command(utf8).status == media::ai::IntentStatus::Ok && PendingSnapshot(e) == MEDIA_OK) return MEDIA_OK;
    ++e->refused;
    return MEDIA_ERROR_REFUSED;
}
media_status media_engine_hear(media_engine_t* e, const char* utf8) {
    if (!e || !utf8) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowHear(e, utf8);
    ++e->commands;
    if (e->host_engine.Hear(utf8) && PendingSnapshot(e) == MEDIA_OK) return MEDIA_OK;
    ++e->refused;
    return MEDIA_ERROR_REFUSED;
}
void media_engine_set_talk(media_engine_t* e, int pressed) {
    if (!e) return;
    if (e->host) e->host_engine.SetTalk(pressed != 0); else WindowSetTalk(e, pressed != 0);
}
media_status media_engine_set_wake(media_engine_t* e, media_wake_policy policy, const char* wake_word) {
    if (!e || policy < MEDIA_WAKE_DEFAULT || policy > MEDIA_WAKE_CUSTOM) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (policy == MEDIA_WAKE_CUSTOM) {
        if (!WordValid(wake_word)) return MEDIA_ERROR_INVALID_ARGUMENT;
        std::memset(e->wake_word, 0, sizeof(e->wake_word)); std::memcpy(e->wake_word, wake_word, std::strlen(wake_word));
        std::memcpy(e->config.wake_word, e->wake_word, sizeof(e->wake_word));
    }
    e->config.wake_policy = policy;
    if (!e->open) return MEDIA_OK;
    // A new name is heard as text at once; its templates are made at the next open.
    const char* word = policy == MEDIA_WAKE_CUSTOM ? e->wake_word : nullptr;
    const bool set = e->host ? e->host_engine.SetWake(policy != MEDIA_WAKE_DISABLED, policy == MEDIA_WAKE_DISABLED, word) : WindowSetWake(e, policy);
    return set ? MEDIA_OK : MEDIA_ERROR_INVALID_ARGUMENT;
}
media_status media_engine_poll_frame(media_engine_t* e, media_frame_signature_t* signature) {
    if (!e || !signature) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowPoll(e, signature);
    FrameSignature s;
    if (!e->host_engine.PollFrame(s)) return MEDIA_ERROR_EMPTY;
    std::memcpy(signature, &s, sizeof(*signature));
    return MEDIA_OK;
}
media_status media_engine_stats(const media_engine_t* e, media_engine_stats_t* stats) {
    if (!e || !stats) return MEDIA_ERROR_INVALID_ARGUMENT;
    std::memset(stats, 0, sizeof(*stats));
    stats->modules = e->config.modules;
    if (!e->ran) return MEDIA_OK;
    if (!e->host) { WindowStats(e, stats); return MEDIA_OK; }
    const auto& h = e->host_engine;
    const auto arena = h.ArenaStats();
    stats->arena_capacity_bytes = arena.capacity_bytes; stats->arena_used_bytes = arena.used_bytes; stats->module_arena_bytes = h.ModuleArenaBytes();
    stats->threads = 0; // Host mode starts none.
    stats->presented = h.Presented(); stats->dropped = h.Dropped();
    stats->signatures = h.Signatures(); stats->signatures_lost = h.SignaturesLost();
    stats->commands = e->commands; stats->refused = e->refused; stats->wake_ignored = h.WakeIgnored(); stats->wake_templates = h.WakeTemplates();
    return MEDIA_OK;
}
uint32_t media_engine_last_error(const media_engine_t* e, int32_t* code) {
    if (code) *code = e ? e->code : 0;
    return e ? e->stage : 0;
}
media_status media_engine_last_error_text(const media_engine_t* e, char* text, size_t bytes) {
    if (!e || !text || !bytes) return MEDIA_ERROR_INVALID_ARGUMENT;
    text[0] = 0;
    if (!e->stage) return MEDIA_OK;
    const char* meaning = media::sdk::StageText(e->host, e->stage);
    if (e->open_failed && e->failed_reason[0])
        std::snprintf(text, bytes, "open failed at stage %u (%s): %s", static_cast<unsigned>(e->stage), meaning, e->failed_reason);
    else if (e->code)
        std::snprintf(text, bytes, "stage %u (%s), code 0x%08x", static_cast<unsigned>(e->stage), meaning, static_cast<unsigned>(e->code));
    else std::snprintf(text, bytes, "stage %u (%s)", static_cast<unsigned>(e->stage), meaning);
    media::sdk::CutUtf8(text, bytes);
    return MEDIA_OK;
}
static_assert(sizeof(media_probe_track_t) == 240 && sizeof(media_probe_t) == 4024 && sizeof(media_open_options_t) == 16, "API 15 layouts (the bindings rely on them)");
media_status media_probe(const char* path, media_probe_t* info) {
    if (!path || !info || info->struct_size != sizeof(media_probe_t)) return MEDIA_ERROR_INVALID_ARGUMENT;
    return media::sdk::Probe(path, *info);
}
media_status media_engine_step(media_engine_t* e, int64_t now_100ns) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    HostDrain(e); // API 12: the commands the host's UI thread posted since the last step.
    const auto step = e->host_engine.Step(now_100ns);
    (void)PendingSnapshot(e); // A spoken "take snapshot" (media_query_snapshot has how it went).
    switch (step) {
    case StepStatus::Playing: return MEDIA_OK;
    case StepStatus::Ended: return MEDIA_ERROR_EMPTY;
    case StepStatus::Failed: break;
    }
    e->stage = e->host_engine.Error();
    return MEDIA_ERROR_PLAYBACK;
}
media_status media_engine_pump_audio(media_engine_t* e, float* out, uint32_t frames, int64_t now_100ns) {
    if (!e || !out) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    (void)e->host_engine.PumpAudio(out, frames, now_100ns);
    return MEDIA_OK;
}
media_status media_engine_feed_voice(media_engine_t* e, const float* samples, uint32_t frames) {
    if (!e || !samples) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host || e->config.voice_input != MEDIA_VOICE_HOST) return MEDIA_ERROR_STATE;
    return e->host_engine.FeedVoice(samples, frames) ? MEDIA_OK : MEDIA_ERROR_STATE;
}
media_status media_engine_state(const media_engine_t* e, media_playback_state_t* state) {
    if (!e || !state) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowState(e, state); // API 12: what the engine's thread published, from any thread.
    std::memset(state, 0, sizeof(*state));
    const auto s = e->host_engine.State();
    state->position_100ns = s.position_100ns; state->duration_100ns = s.duration_100ns; state->epoch = s.epoch;
    state->paused = s.paused; state->muted = s.muted; state->tracking = static_cast<int32_t>(s.tracking);
    state->volume = s.volume; state->speed = s.speed; state->view_zoom = s.view_zoom; state->view_x = s.view_x; state->view_y = s.view_y;
    state->audio_track = s.audio_track; state->audio_tracks = s.audio_tracks; std::memcpy(state->audio_language, s.audio_language, 4);
    state->presented = s.presented; state->dropped = s.dropped; state->underruns = s.underruns; state->saccade = s.saccade;
    const auto bit = [](bool on, uint32_t value) { return on ? value : 0u; };
    state->orientation = (s.quarter_turns & static_cast<uint32_t>(MEDIA_ORIENTATION_TURNS)) | bit(s.mirrored, MEDIA_ORIENTATION_MIRRORED) |
                         bit(s.auto_level, MEDIA_ORIENTATION_AUTO_LEVEL) | bit(s.auto_crop, MEDIA_ORIENTATION_AUTO_CROP);
    state->level_degrees = s.level_degrees;
    return MEDIA_OK;
}
// ---- Embedding (API 12; docs/sdk.md, "Embedding: API 12") ----
uint32_t media_api_version(void) { return MEDIA_API_VERSION; }
media_status media_engine_post_command(media_engine_t* e, const char* utf8, uint32_t command_id) {
    if (!e || !utf8) return MEDIA_ERROR_INVALID_ARGUMENT;
    const auto n = std::strlen(utf8);
    if (n >= sizeof(HostCommand::text)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowPostCommand(e, utf8, command_id);
    auto* c = e->host_commands.AcquireWriteSlot();
    if (!c) return MEDIA_ERROR_REFUSED;
    std::memcpy(c->text, utf8, n); c->text[n] = 0; c->length = static_cast<uint32_t>(n); c->id = command_id;
    c->op = 0; c->value = 0;
    return e->host_commands.CommitWriteSlot() ? MEDIA_OK : MEDIA_ERROR_REFUSED;
}
media_status media_engine_track_count(const media_engine_t* e, uint32_t* count) {
    if (!e || !count) return MEDIA_ERROR_INVALID_ARGUMENT;
    *count = 0;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (e->host) { *count = e->host_engine.CatalogueCount(); return MEDIA_OK; } // API 17 (a live sensor: none).
    return WindowTrackCount(e, *count);
}
media_status media_engine_track_info(const media_engine_t* e, uint32_t index, media_track_info_t* info) {
    static_assert(sizeof(media_track_info_t) == 88, "API 16 layout (the bindings rely on it)");
    if (!e || !info || info->struct_size != sizeof(*info)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (e->host) {
        // API 17: host mode's catalogue (HostEngine::CatalogueAt), the windowed engine's fields.
        const auto& h = e->host_engine;
        if (index >= h.CatalogueCount()) return MEDIA_ERROR_INVALID_ARGUMENT;
        const auto& t = h.CatalogueAt(index);
        using media::demux::TrackKind;
        const auto size = info->struct_size; std::memset(info, 0, sizeof(*info)); info->struct_size = size;
        info->id = t.id;
        info->kind = t.kind == TrackKind::Video ? MEDIA_TRACK_VIDEO : t.kind == TrackKind::Audio ? MEDIA_TRACK_AUDIO : t.kind == TrackKind::Subtitle ? MEDIA_TRACK_SUBTITLE : MEDIA_TRACK_OTHER;
        if (t.kind == TrackKind::Subtitle && t.id == 0) std::memcpy(info->codec, "srt", 3); else media::sdk::CodecText(t.codec, info->codec);
        std::memcpy(info->language, t.language, 3); std::memcpy(info->title, t.title, sizeof(info->title) - 1);
        info->channels = t.channels; info->sample_rate = t.rate; info->width = t.width; info->height = t.height;
        info->selectable = t.selectable ? 1u : 0u; info->is_default = t.is_default ? 1u : 0u;
        if (t.kind == TrackKind::Video) info->active = info->selectable;
        else if (t.kind == TrackKind::Audio) info->active = t.selectable && h.ActiveAudio() == t.id ? 1u : 0u;
        else if (t.kind == TrackKind::Subtitle) info->active = t.selectable && h.ActiveSubtitles() == static_cast<std::int64_t>(t.id) ? 1u : 0u;
        return MEDIA_OK;
    }
    return WindowTrackInfo(e, index, *info);
}
media_status media_engine_post_control(media_engine_t* e, uint32_t control, double value, uint32_t command_id) {
    using media::engine::HostControl;
    if (!e || control == 0 || control > media::engine::kLastControl ||
        !media::engine::ControlValueValid(static_cast<HostControl>(control), value)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowPostControl(e, control, value, command_id);
    auto* c = e->host_commands.AcquireWriteSlot();
    if (!c) return MEDIA_ERROR_REFUSED;
    c->text[0] = 0; c->length = 0; c->id = command_id; c->op = control; c->value = value;
    return e->host_commands.CommitWriteSlot() ? MEDIA_OK : MEDIA_ERROR_REFUSED;
}
media_status media_engine_poll_event(media_engine_t* e, media_event_t* out) {
    static_assert(sizeof(media_event_t) == 80 && static_cast<int>(MEDIA_EVENT_ENDED) == static_cast<int>(HostEvent::kEnded) &&
                  static_cast<int>(MEDIA_EVENT_FULLSCREEN_REQUEST) == static_cast<int>(HostEvent::kFullscreen) &&
                  static_cast<int>(MEDIA_ENDED_LIMIT) == static_cast<int>(HostEvent::kLimit) && static_cast<int>(MEDIA_ENDED_CLOSED) == static_cast<int>(HostEvent::kClosed),
                  "the C event is the engine's");
    if (!e || !out || out->struct_size != sizeof(*out)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->ran) return MEDIA_ERROR_STATE; // Readable after close too, until the next open: kEnded is the last.
    HostEvent h{};
    if (e->host) {
        const auto* slot = e->host_events.PeekReadSlot();
        if (!slot) return MEDIA_ERROR_EMPTY;
        h = *slot; (void)e->host_events.ReleaseReadSlot();
    } else if (const auto status = WindowPollEvent(e, h); status != MEDIA_OK) return status;
    std::memset(out, 0, sizeof(*out)); out->struct_size = sizeof(*out);
    out->kind = h.kind; out->time_100ns = h.time; out->position_100ns = h.position; out->duration_100ns = h.duration; out->latency_100ns = h.latency;
    out->command_id = h.id; out->paused = h.paused; out->muted = h.muted; out->volume = h.volume; out->speed = h.speed;
    out->width = h.width; out->height = h.height; out->reason = h.reason; out->lost = h.lost;
    out->status = h.kind == static_cast<std::uint32_t>(HostEvent::kCommand) ? (h.status ? MEDIA_ERROR_REFUSED : MEDIA_OK) :
                  h.kind == static_cast<std::uint32_t>(HostEvent::kEnded) ? (h.status ? MEDIA_ERROR_PLAYBACK : MEDIA_OK) : MEDIA_OK;
    return MEDIA_OK;
}
// ---- Texture output (API 13; docs/sdk.md, "Texture output: API 13") ----
// API 13's structs (72 and 32 bytes on x64) and API 14's (80 and 40: the colour space, the SDR white, the picture's transfer).
constexpr std::size_t kApi13TexturesBytes = offsetof(media_output_textures_t, color_space), kApi13FrameBytes = offsetof(media_output_frame_t, transfer);
static bool TexturesSize(const media_output_textures_t* t) noexcept { return t && (t->struct_size == sizeof(*t) || t->struct_size == kApi13TexturesBytes); }
static bool FrameSize(const media_output_frame_t* f) noexcept { return f && (f->struct_size == sizeof(*f) || f->struct_size == kApi13FrameBytes); }
media_status media_engine_query_output_textures(media_engine_t* e, media_output_textures_t* out) {
    static_assert(sizeof(void*) != 8 || (sizeof(media_output_textures_t) == 80 && offsetof(media_output_textures_t, textures) == 24 && kApi13TexturesBytes == 72 &&
                  sizeof(media_output_frame_t) == 40 && offsetof(media_output_frame_t, fence_value) == 8 && kApi13FrameBytes == 32), "the x64 layouts bindings use");
    if (!e || !TexturesSize(out)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    return e->host ? HostOutputTextures(e, out) : WindowOutputTextures(e, out);
}
media_status media_engine_acquire_output_frame(media_engine_t* e, media_output_frame_t* out) {
    if (!e || !FrameSize(out)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    return e->host ? HostAcquireOutput(e, out) : WindowAcquireOutput(e, out);
}
media_status media_engine_release_output_frame(media_engine_t* e, uint32_t index, uint64_t host_fence_value) {
    if (!e || index >= 3) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    return e->host ? HostReleaseOutput(e, index, host_fence_value) : WindowReleaseOutput(e, index, host_fence_value);
}
media_status media_engine_resize_output(media_engine_t* e, uint32_t width, uint32_t height) {
    if (!e || width < 16 || height < 16 || width > 8192 || height > 8192) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    return e->host ? HostResizeOutput(e, width, height) : WindowResizeOutput(e, width, height);
}
uint32_t media_query_video_decoder(const media_engine_t* e) {
    if (!e || !e->open) return MEDIA_DECODER_NONE;
    if (!e->host) return MEDIA_DECODER_HARDWARE; // The windowed engine decodes on the GPU (D3D12 video).
    switch (e->host_engine.VideoDecoder()) {
    case HostEngine::Decoder::Hardware: return MEDIA_DECODER_HARDWARE;
    case HostEngine::Decoder::Host: return MEDIA_DECODER_HOST;
    case HostEngine::Decoder::Software: return MEDIA_DECODER_SOFTWARE;
    case HostEngine::Decoder::None: break;
    }
    return MEDIA_DECODER_NONE;
}
void media_sensor_config_init(media_sensor_config_t* config) {
    if (!config) return;
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
}
media_status media_engine_open_sensor(media_engine_t* e, const media_sensor_config_t* sensor) {
    if (!e || !sensor || sensor->struct_size != sizeof(*sensor)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (e->open || !e->host) return MEDIA_ERROR_STATE;
    if (sensor->options & ~static_cast<uint32_t>(MEDIA_SENSOR_GPU | MEDIA_SENSOR_STABILIZE | MEDIA_SENSOR_GPU_10BIT | MEDIA_SENSOR_GPU_FIT)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if ((sensor->options & (MEDIA_SENSOR_GPU_10BIT | MEDIA_SENSOR_GPU_FIT)) && !(sensor->options & MEDIA_SENSOR_GPU)) return MEDIA_ERROR_INVALID_ARGUMENT;
    e->sensor_options = sensor->options;
    const auto status = HostOpen(e, nullptr, sensor);
    if (status != MEDIA_OK) return status;
    if (sensor->options & MEDIA_SENSOR_GPU) {
        // The GPU path needs a camera; without a device the sensor is not opened (the host asked for the GPU's passes).
        const auto gpu = sensor->width ? GpuOpen(e, *sensor) : MEDIA_ERROR_INVALID_ARGUMENT;
        if (gpu != MEDIA_OK) { e->host_engine.Close(); e->open = false; return gpu; }
    }
    if ((sensor->options & MEDIA_SENSOR_STABILIZE) && e->host_engine.SetStabilization(true) != media::ai::IntentStatus::Ok) {
        GpuClose(e); e->host_engine.Close(); e->open = false; return MEDIA_ERROR_UNSUPPORTED;
    }
    return MEDIA_OK;
}
media_status media_engine_ingest_video_frame(media_engine_t* e, const media_sensor_frame_t* f) {
    if (!e || !f || f->struct_size != sizeof(*f)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    SensorFrame frame;
    frame.format = f->format; frame.width = f->width; frame.height = f->height;
    frame.planes[0] = f->planes[0]; frame.planes[1] = f->planes[1]; frame.strides[0] = f->strides[0]; frame.strides[1] = f->strides[1];
    frame.pts_100ns = f->timestamp_100ns;
    if (f->flags & MEDIA_SENSOR_IMU) frame.ego = {f->imu_yaw, f->imu_pitch, f->imu_roll, true};
    if (!e->host_engine.IngestVideo(frame)) return MEDIA_ERROR_INVALID_ARGUMENT;
    GpuIngest(e, *f);
    return MEDIA_OK;
}
media_status media_engine_ingest_audio_samples(media_engine_t* e, const void* samples, media_sample_format format, uint32_t frames, int64_t timestamp_100ns) {
    if (!e || !samples) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    return e->host_engine.IngestAudio(samples, static_cast<std::uint32_t>(format), frames, timestamp_100ns) ? MEDIA_OK : MEDIA_ERROR_INVALID_ARGUMENT;
}
media_status media_query_frame_telemetry(const media_engine_t* e, media_frame_telemetry_t* out) {
    // API 5's struct (through horizon_y and focus_x), API 6's (through reserved), API 7's and 8's (through objects) or API 9's:
    // the answer is made whole, then the host's part copied.
    constexpr std::size_t kApi5Bytes = 112, kApi6Bytes = offsetof(media_frame_telemetry_t, object_count), kApi7Bytes = offsetof(media_frame_telemetry_t, vibration_hz);
    constexpr std::size_t kApi9Bytes = offsetof(media_frame_telemetry_t, vibrometry_flags);
    static_assert(offsetof(media_frame_telemetry_t, estimated_horizon_deg) == 104 && kApi6Bytes == 136 && sizeof(media_detected_object_t) == 64 &&
                  kApi7Bytes == kApi6Bytes + 8 + 64 * MEDIA_MAX_OBJECTS && kApi7Bytes == 400 && kApi9Bytes == kApi7Bytes + 48 && sizeof(media_frame_telemetry_t) == kApi9Bytes + 24);
    if (!e || !out || (out->struct_size != sizeof(*out) && out->struct_size != kApi5Bytes && out->struct_size != kApi6Bytes && out->struct_size != kApi7Bytes &&
                       out->struct_size != kApi9Bytes))
        return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return MEDIA_ERROR_UNSUPPORTED;
    const auto h = e->host_engine.Telemetry();
    media_frame_telemetry_t whole{}; auto* t = &whole;
    t->struct_size = out->struct_size;
    const auto& p = h.perception;
    t->pts_100ns = p.pts_100ns; t->sequence = p.sequence;
    const auto flag = [](bool on, unsigned bit) { return on ? bit : 0u; };
    t->flags = flag(p.flow, MEDIA_TELEMETRY_FLOW) | flag(p.looming, MEDIA_TELEMETRY_LOOMING) | flag(p.ego, MEDIA_TELEMETRY_EGO) |
               flag(h.sound_known, MEDIA_TELEMETRY_SOUND) | flag(h.saccade, MEDIA_TELEMETRY_SACCADE) |
               flag(e->host_engine.Stabilizing(), MEDIA_TELEMETRY_STABILIZED) | flag(p.ground, MEDIA_TELEMETRY_GROUND) | flag(p.looming && p.fovea, MEDIA_TELEMETRY_FOVEA) |
               flag(h.horizon, MEDIA_TELEMETRY_HORIZON) | flag(h.bars, MEDIA_TELEMETRY_BARS);
    t->motion_x = p.motion_x; t->motion_y = p.motion_y; t->flow_coverage = p.coverage; t->divergence = p.divergence; t->ttc_seconds = p.ttc_seconds;
    t->hazard_x = p.hazard_x; t->hazard_y = p.hazard_y; t->salient_fraction = p.salient_fraction;
    t->sound_azimuth = h.sound_known ? h.sound.azimuth : 0; t->sound_level_db = h.sound_known ? h.sound.level_db : -144; t->sound_pts_100ns = h.sound_known ? h.sound.pts_100ns : -1;
    t->sound_events = h.sound_known ? h.sound.sequence : 0; t->saccades = h.saccades;
    t->view_zoom = h.view_zoom; t->view_x = h.view_x; t->view_y = h.view_y; t->view_roll = h.view_roll;
    t->horizon_y = p.ground ? p.horizon_y : -1.0f; t->focus_x = p.ground ? p.focus_x : -1.0f;
    t->estimated_horizon_deg = h.horizon ? h.horizon_tilt : 0.0f;
    t->active_left = h.active.left; t->active_top = h.active.top; t->active_right = h.active.right; t->active_bottom = h.active.bottom;
    t->level_degrees = e->host_engine.OrientationNow().level;
    // API 7: the objects, the most urgent first, and the one target tracking follows.
    static_assert(MEDIA_MAX_OBJECTS == media::video::kMaxPerceivedObjects);
    t->object_count = p.object_count; t->target_track_id = h.target_id;
    if (p.object_count) t->flags |= MEDIA_TELEMETRY_OBJECTS;
    for (unsigned i = 0; i < p.object_count; ++i) {
        const auto& o = p.objects[i]; auto& d = t->objects[i];
        d.track_id = o.track_id;
        d.flags = flag(o.approaching, MEDIA_OBJECT_APPROACHING) | flag(o.fovea && o.divergence > 0, MEDIA_OBJECT_FOVEA) |
                  flag(h.target_id && o.track_id == h.target_id, MEDIA_OBJECT_TARGET);
        d.x = o.x; d.y = o.y; d.width = o.width; d.height = o.height; d.velocity_x = o.vx; d.velocity_y = o.vy;
        d.ttc_seconds = o.ttc_seconds; d.divergence = o.divergence; d.saliency = o.saliency; d.energy = o.energy;
        d.support = o.support; d.tiles = o.tiles; d.age = o.age;
    }
    // API 9: remote vibrometry.
    const auto& v = h.vibrometry;
    t->vibration_hz = v.vibration_hz; t->vibrometry_confidence = v.confidence; t->vibration_amplitude = v.amplitude; t->vibrometry_channel = v.channel;
    t->vital_bpm = v.vital_bpm; t->vital_confidence = v.vital_confidence; t->vital_channel = v.vital_channel; t->vibrometry_seconds = v.seconds;
    t->vibrometry_left = static_cast<float>(h.vibrometry_region.left); t->vibrometry_top = static_cast<float>(h.vibrometry_region.top);
    t->vibrometry_right = static_cast<float>(h.vibrometry_region.right); t->vibrometry_bottom = static_cast<float>(h.vibrometry_region.bottom);
    t->flags |= flag(v.confidence > 0, MEDIA_TELEMETRY_VIBRATION) | flag(v.vital_confidence > 0, MEDIA_TELEMETRY_VITALS);
    // API 10: the virtual IMU, and vibrometry following its target.
    t->vibrometry_flags = flag(h.vibrometry_following, MEDIA_VIBROMETRY_FOLLOWING);
    t->flags |= flag(h.virtual_imu, MEDIA_TELEMETRY_VIRTUAL_IMU);
    const auto& m = h.virtual_motion;
    t->ego_dx = m.valid ? m.dx : 0.0f; t->ego_dy = m.valid ? m.dy : 0.0f; t->ego_turn = m.valid ? m.turn : 0.0f; t->ego_tiles = m.tiles; t->ego_inliers = m.inliers;
    std::memcpy(out, t, out->struct_size);
    return p.sequence || e->host_engine.Surveyed() ? MEDIA_OK : MEDIA_ERROR_EMPTY;
}
media_status media_query_saliency_grid(const media_engine_t* e, media_saliency_grid_t* g) {
    if (!e || !g || g->struct_size != sizeof(*g)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return MEDIA_ERROR_UNSUPPORTED;
    const auto& p = e->host_engine.Perceiver();
    std::memset(g, 0, sizeof(*g)); g->struct_size = sizeof(*g);
    if (!p.Configured() || !p.Result().sequence) return MEDIA_ERROR_EMPTY;
    g->pts_100ns = p.Result().pts_100ns; g->salient_tiles = p.Result().salient;
    const float w = static_cast<float>(p.Width()), h = static_cast<float>(p.Height());
    for (unsigned t = 0; t < MEDIA_SALIENCY_TILES * MEDIA_SALIENCY_TILES; ++t) {
        g->weight[t] = p.Saliency(t); g->flow_valid[t] = p.FlowValid(t) ? 1 : 0;
        g->flow_x[t] = p.FlowX(t) / w; g->flow_y[t] = p.FlowY(t) / h;
        if (p.Salient(t)) g->mask[t / MEDIA_SALIENCY_TILES] = static_cast<uint16_t>(g->mask[t / MEDIA_SALIENCY_TILES] | (1u << (t % MEDIA_SALIENCY_TILES)));
    }
    return MEDIA_OK;
}
media_status media_engine_set_stabilization(media_engine_t* e, int on) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowStabilization(e, on != 0); // API 11: the window's view.
    const auto status = e->host_engine.SetStabilization(on != 0);
    return status == media::ai::IntentStatus::Ok || status == media::ai::IntentStatus::NoContext ? MEDIA_OK : MEDIA_ERROR_UNSUPPORTED;
}
void media_picture_controls_init(media_picture_controls_t* c) {
    if (!c) return;
    std::memset(c, 0, sizeof(*c));
    c->struct_size = sizeof(*c); c->contrast = c->saturation = c->gamma = c->micro_motion = 1; c->reconstruction = MEDIA_RECONSTRUCT_EDGE;
}
media_status media_engine_set_picture_controls(media_engine_t* e, const media_picture_controls_t* c) {
    if (!e || !c || c->struct_size != sizeof(*c) || c->reconstruction > MEDIA_RECONSTRUCT_NEURAL) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    return GpuControls(e, *c);
}
media_status media_query_gpu_image(media_engine_t* e, media_gpu_image_t* image) {
    // API 7's struct (through reserved) or API 8's (the picture's rectangle): the answer is made whole, then the host's part copied.
    constexpr std::size_t kApi7Bytes = offsetof(media_gpu_image_t, picture_left);
    if (!e || !image || (image->struct_size != sizeof(*image) && image->struct_size != kApi7Bytes)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    media_gpu_image_t whole{};
    const auto status = GpuImage(e, &whole);
    if (status != MEDIA_OK) return status;
    whole.struct_size = image->struct_size;
    std::memcpy(image, &whole, image->struct_size);
    return MEDIA_OK;
}
media_status media_engine_set_snapshot_folder(media_engine_t* e, const char* folder) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    const auto n = folder ? std::strlen(folder) : 0;
    if (n >= sizeof(e->snapshot_folder)) return MEDIA_ERROR_INVALID_ARGUMENT;
    std::memset(e->snapshot_folder, 0, sizeof(e->snapshot_folder));
    if (n) std::memcpy(e->snapshot_folder, folder, n);
    if (e->open && !e->host) WindowSnapshotFolder(e);
    return MEDIA_OK;
}
media_status media_engine_save_snapshot(media_engine_t* e, const char* path) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    return e->host ? HostSnapshot(e, path) : WindowSnapshot(e, path);
}
media_status media_engine_snapshot_to_memory(media_engine_t* e, media_snapshot_buffer_t* b) {
    if (!e || !b || b->struct_size != sizeof(*b) || !b->pixels || (b->format != MEDIA_PIXELS_BGRA8 && b->format != MEDIA_PIXELS_RGBA8)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    const media::video::SnapshotMemory memory{static_cast<std::uint8_t*>(b->pixels), b->capacity_bytes, b->stride, b->format == MEDIA_PIXELS_RGBA8};
    media::video::SnapshotResult r{};
    media_status status = MEDIA_OK;
    if (e->host) { status = HostSnapshot(e, nullptr, &memory); r = e->last_memory_; }
    else status = WindowSnapshotMemory(e, memory, r);
    b->width = r.width; b->height = r.height; b->pts_100ns = r.pts_100ns; b->frame_hash = r.hash;
    return status;
}
media_status media_engine_attach_subtitles(media_engine_t* e, const char* path) {
    if (!e || !path || !*path) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!(e->config.modules & MEDIA_MODULE_SUBTITLES)) return MEDIA_ERROR_UNSUPPORTED;
    if (e->host) return e->host_engine.AttachSubtitles(path) ? MEDIA_OK : MEDIA_ERROR_OPEN;
    return WindowAttachSubtitles(e, path);
}
media_status media_query_snapshot(const media_engine_t* e, media_snapshot_t* snapshot) {
    if (!e || !snapshot || snapshot->struct_size != sizeof(*snapshot)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->ran) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowSnapshotQuery(e, snapshot);
    if (!e->snapshot.struct_size) return MEDIA_ERROR_EMPTY;
    *snapshot = e->snapshot;
    return MEDIA_OK;
}
media_status media_engine_set_orientation(media_engine_t* e, uint32_t quarter_turns, float level_degrees) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    const auto status = e->host_engine.SetOrientation(quarter_turns, level_degrees);
    return status == media::ai::IntentStatus::Ok ? MEDIA_OK : status == media::ai::IntentStatus::OutOfRange ? MEDIA_ERROR_INVALID_ARGUMENT : MEDIA_ERROR_UNSUPPORTED;
}
media_status media_engine_set_auto_level(media_engine_t* e, int on) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    const auto status = e->host_engine.SetAutoLevel(on != 0);
    return status == media::ai::IntentStatus::Ok || status == media::ai::IntentStatus::NoContext ? MEDIA_OK : MEDIA_ERROR_UNSUPPORTED;
}
media_status media_engine_set_auto_crop(media_engine_t* e, int on) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    const auto status = e->host_engine.SetAutoCrop(on != 0);
    return status == media::ai::IntentStatus::Ok || status == media::ai::IntentStatus::NoContext ? MEDIA_OK : MEDIA_ERROR_UNSUPPORTED;
}
media_status media_engine_track_object(media_engine_t* e, uint32_t track_id) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    using media::ai::IntentStatus;
    switch (e->host_engine.TrackObject(track_id)) {
    case IntentStatus::Ok: return MEDIA_OK;
    case IntentStatus::NotFound: return MEDIA_ERROR_EMPTY;
    case IntentStatus::NoContext: return MEDIA_ERROR_REFUSED;
    default: return MEDIA_ERROR_UNSUPPORTED;
    }
}
media_status media_engine_set_audio_saccade(media_engine_t* e, int on) {
    if (!e) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open || !e->host) return MEDIA_ERROR_STATE;
    const auto status = e->host_engine.SetSaccade(on != 0);
    return status == media::ai::IntentStatus::Ok || (!on && status == media::ai::IntentStatus::NoContext) ? MEDIA_OK : MEDIA_ERROR_UNSUPPORTED;
}
// ---- API 9: the event camera, bitstream perception, tensor export (docs/sdk.md) ----
void media_event_camera_config_init(media_event_camera_config_t* c) {
    if (!c) return;
    std::memset(c, 0, sizeof(*c));
    c->struct_size = sizeof(*c); c->contrast_threshold = 0.15f; c->max_events_per_pixel = 4;
}
media_status media_engine_set_event_camera(media_engine_t* e, const media_event_camera_config_t* c) {
    if (!e || (c && (c->struct_size != sizeof(*c) || c->flags & ~static_cast<uint32_t>(MEDIA_EVENTS_UNSTABILIZED) || c->contrast_threshold < 0 ||
                     c->contrast_threshold > 2 || c->max_events_per_pixel > 16))) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    media::video::EventCameraOptions o;
    if (c) {
        o.contrast_threshold = c->contrast_threshold > 0 ? c->contrast_threshold : 0.15f;
        o.max_per_pixel = c->max_events_per_pixel ? c->max_events_per_pixel : 4u;
        o.stabilized = !(c->flags & MEDIA_EVENTS_UNSTABILIZED);
    }
    if (!e->host) return WindowEventCamera(e, c ? &o : nullptr); // API 10: the window's, on its tracking grids.
    const auto status = e->host_engine.SetEventCamera(c ? &o : nullptr);
    return status == media::ai::IntentStatus::Ok || status == media::ai::IntentStatus::NoContext ? MEDIA_OK : MEDIA_ERROR_UNSUPPORTED;
}
media_status media_poll_event_spikes(media_engine_t* e, media_event_spike_t* spikes, uint32_t capacity, uint32_t* count) {
    static_assert(sizeof(media_event_spike_t) == sizeof(media::video::EventSpike) && offsetof(media_event_spike_t, timestamp_us) == offsetof(media::video::EventSpike, timestamp_us) &&
                  offsetof(media_event_spike_t, contrast) == offsetof(media::video::EventSpike, contrast), "the C event is the engine's");
    if (count) *count = 0;
    if (!e || !spikes || !count) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowPollEvents(e, reinterpret_cast<media::video::EventSpike*>(spikes), capacity, *count);
    *count = e->host_engine.Events().Poll(reinterpret_cast<media::video::EventSpike*>(spikes), capacity);
    return *count ? MEDIA_OK : MEDIA_ERROR_EMPTY;
}
media_status media_query_event_camera(const media_engine_t* e, media_event_camera_state_t* state) {
    if (!e || !state || state->struct_size != sizeof(*state)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    if (!e->host) return WindowEventState(e, state);
    const auto& c = e->host_engine.Events();
    if (!c.Configured()) return MEDIA_ERROR_UNSUPPORTED;
    std::memset(state, 0, sizeof(*state)); state->struct_size = sizeof(*state);
    state->on = e->host_engine.EventCameraOn() ? 1 : 0; state->width = c.Width(); state->height = c.Height(); state->scale = c.Scale();
    state->pictures = c.Pictures(); state->events = c.Emitted(); state->dropped = c.Dropped(); state->rebases = c.Rebases(); state->last_events = c.LastCount();
    return MEDIA_OK;
}
media_status media_bitstream_inspector_init(media_bitstream_inspector_t* inspector, uint32_t codec, const uint8_t* config, uint32_t config_bytes, uint32_t nal_length_bytes) {
    using media::codec::BitstreamPerception;
    static_assert(sizeof(BitstreamPerception) <= sizeof(media_bitstream_inspector_t) && alignof(BitstreamPerception) <= alignof(media_bitstream_inspector_t) &&
                  std::is_trivially_destructible_v<BitstreamPerception>, "the inspector lives in the caller's storage");
    if (!inspector || (codec != MEDIA_CODEC_H264 && codec != MEDIA_CODEC_HEVC) || (config_bytes && !config)) return MEDIA_ERROR_INVALID_ARGUMENT;
    auto* p = ::new (static_cast<void*>(inspector)) BitstreamPerception();
    return p->Configure(codec == MEDIA_CODEC_H264 ? BitstreamPerception::kH264 : BitstreamPerception::kHevc, config, config_bytes, nal_length_bytes)
               ? MEDIA_OK : MEDIA_ERROR_INVALID_ARGUMENT;
}
media_status media_inspect_bitstream_packet(media_bitstream_inspector_t* inspector, const uint8_t* data, uint32_t bytes, media_bitstream_info_t* info) {
    if (!inspector || !data || !info || info->struct_size != sizeof(*info)) return MEDIA_ERROR_INVALID_ARGUMENT;
    auto* p = std::launder(reinterpret_cast<media::codec::BitstreamPerception*>(inspector));
    if (!p->Codec()) return MEDIA_ERROR_STATE;
    media::codec::BitstreamFrame f;
    const bool whole = p->Inspect(data, bytes, f);
    std::memset(info, 0, sizeof(*info)); info->struct_size = sizeof(*info);
    if (!whole) return MEDIA_ERROR_INVALID_ARGUMENT;
    info->index = f.index; info->bytes = f.bytes; info->slices = f.slices;
    info->picture_type = f.type == 'I' ? MEDIA_PICTURE_TYPE_I : f.type == 'P' ? MEDIA_PICTURE_TYPE_P : f.type == 'B' ? MEDIA_PICTURE_TYPE_B : MEDIA_PICTURE_TYPE_NONE;
    const auto bit = [](bool on, unsigned value) { return on ? value : 0u; };
    info->flags = bit(f.key, MEDIA_BITSTREAM_KEY) | bit(f.parameter_sets, MEDIA_BITSTREAM_PARAMETER_SETS) | bit(f.scene_cut >= 0.5f, MEDIA_BITSTREAM_SCENE_CUT) |
                  bit(f.motion_burst, MEDIA_BITSTREAM_MOTION_BURST) | bit(f.qp >= 0, MEDIA_BITSTREAM_QP) | bit(f.revised, MEDIA_BITSTREAM_REVISION);
    info->qp = f.qp; info->complexity = f.complexity; info->activity = f.activity; info->burst = f.burst; info->scene_cut_probability = f.scene_cut;
    info->width = p->Width(); info->height = p->Height();
    info->revised_index = f.revised_index; info->revised_scene_cut_probability = f.revised_scene_cut;
    return f.slices ? MEDIA_OK : MEDIA_ERROR_EMPTY;
}
void media_tensor_request_init(media_tensor_request_t* r) {
    if (!r) return;
    std::memset(r, 0, sizeof(*r));
    r->struct_size = sizeof(*r); r->width = r->height = 224; r->channels = 3; r->format = MEDIA_TENSOR_F32; r->normalize = MEDIA_TENSOR_UNIT; r->region = MEDIA_TENSOR_FOVEA;
}
namespace {
bool TensorSpecOf(const media_tensor_request_t* r, media::video::TensorSpec& spec) noexcept {
    using media::video::TensorSpec;
    if (!r || r->struct_size != sizeof(*r) || r->flags & ~static_cast<uint32_t>(MEDIA_TENSOR_BGR) || r->region > MEDIA_TENSOR_REGION) return false;
    spec.width = r->width; spec.height = r->height; spec.channels = r->channels;
    spec.format = r->format == MEDIA_TENSOR_F16 ? TensorSpec::kFloat16 : r->format == MEDIA_TENSOR_F32 ? TensorSpec::kFloat32 : static_cast<TensorSpec::Format>(0);
    spec.normalize = static_cast<TensorSpec::Normalize>(r->normalize); spec.bgr = (r->flags & MEDIA_TENSOR_BGR) != 0;
    return media::video::TensorBytes(spec) != 0;
}
}
size_t media_tensor_bytes(const media_tensor_request_t* r) {
    media::video::TensorSpec spec;
    return TensorSpecOf(r, spec) ? media::video::TensorBytes(spec) : 0;
}
media_status media_export_foveated_tensor(media_engine_t* e, const media_tensor_request_t* r, void* tensor, size_t bytes, media_tensor_info_t* info) {
    media::video::TensorSpec spec;
    if (!e || !tensor || !TensorSpecOf(r, spec) || (info && info->struct_size != sizeof(*info))) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->open) return MEDIA_ERROR_STATE;
    const auto need = media::video::TensorBytes(spec);
    if (bytes < need) return MEDIA_ERROR_STORAGE;
    if (!e->host) return WindowTensor(e, spec, *r, tensor, bytes, info); // API 10: the window's next picture, from the GPU.
    const std::uint8_t *y = nullptr, *cb = nullptr, *cr = nullptr; std::uint32_t ys = 0, cs = 0, w = 0, h = 0; std::int64_t pts = -1;
    if (!e->host_engine.ShownPicture(y, cb, cr, ys, cs, w, h, pts)) return e->host_engine.Presented() ? MEDIA_ERROR_UNSUPPORTED : MEDIA_ERROR_EMPTY;
    using Region = HostEngine::TensorRegion;
    const Region kind = r->region == MEDIA_TENSOR_VIEW ? Region::View : r->region == MEDIA_TENSOR_FULL ? Region::Full : r->region == MEDIA_TENSOR_REGION ? Region::Custom : Region::Fovea;
    media::video::AreaRect area{};
    if (!e->host_engine.TensorArea(kind, {r->left, r->top, r->right, r->bottom}, static_cast<double>(spec.width) / spec.height, area)) return MEDIA_ERROR_INVALID_ARGUMENT;
    const media::video::Planes420 planes{y, cb, cr, ys, cs, w, h};
    if (!media::video::ExportTensor(planes, area, spec, tensor, bytes)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (info) {
        std::memset(info, 0, sizeof(*info)); info->struct_size = sizeof(*info);
        info->region = r->region; info->left = static_cast<float>(area.left); info->top = static_cast<float>(area.top);
        info->right = static_cast<float>(area.right); info->bottom = static_cast<float>(area.bottom);
        info->pts_100ns = pts; info->bytes = need; info->flags = 0;
        const auto state = e->host_engine.State();
        info->target_track_id = kind == Region::Fovea && state.tracking == 1 ? e->host_engine.TargetId() : 0u;
    }
    return MEDIA_OK;
}
}
