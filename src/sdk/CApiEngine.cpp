// The C SDK's windowed engine (docs/sdk.md), on Windows: a PlaybackCoordinator in the host's storage. No heap: paths are
// converted into fixed buffers, and each open constructs a fresh coordinator in the same storage (a coordinator plays one file).
#include "CApiEngine.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace {
using media::engine::FrameSignature;
using media::engine::PlaybackCoordinator;
// UTF-8 to UTF-16 into a fixed buffer; false when it does not fit or is not UTF-8.
bool Wide(const char* utf8, wchar_t* out, int capacity) noexcept {
    return utf8 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, capacity) > 0;
}
void Forward(void* context, const FrameSignature& s) noexcept {
    auto* self = static_cast<media_engine*>(context);
    media_frame_signature_t c; std::memcpy(&c, &s, sizeof(c));
    self->config.on_frame(self->config.on_frame_context, &c);
}
std::size_t Speak(void* context, const char* text, unsigned voice, int rate, float* out, std::size_t capacity) noexcept {
    const auto& c = static_cast<media_engine*>(context)->config;
    return c.speak ? c.speak(c.speak_context, text, voice, rate, out, capacity) : 0;
}
}
namespace media::sdk {
media_status WindowOpen(media_engine* e, const char* path) noexcept {
    if (!Wide(path, e->path, 2048)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (e->config.parent_window && !IsWindow(static_cast<HWND>(e->config.parent_window))) return MEDIA_ERROR_INVALID_ARGUMENT; // API 12.
    if (e->subtitles[0] && !Wide(e->subtitles, e->subtitles_wide, 1024)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (e->ran) { e->coordinator.~PlaybackCoordinator(); ::new (&e->coordinator) PlaybackCoordinator(); } // One file each.
    media::engine::PlaybackOptions o;
    const auto& c = e->config;
    o.visible = c.visible != 0; o.activate = c.visible != 0; o.isolate_input = c.visible == 0;
    if (c.output_flags & MEDIA_OUTPUT_TEXTURE) { // API 13: nothing on the screen; the pictures go to the host's renderer.
        o.texture_width = c.output_width; o.texture_height = c.output_height; o.visible = o.activate = false; o.isolate_input = true;
        o.texture_format = OutputFormat(c.output_format); // API 14.
    }
    o.maximum_seconds = c.maximum_seconds;
    o.modules = c.modules;
    o.subtitle_path = e->subtitles[0] ? e->subtitles_wide : nullptr;
    o.voice = c.voice_input == MEDIA_VOICE_MICROPHONE ? media::engine::VoiceInput::Microphone :
              c.voice_input == MEDIA_VOICE_LOOPBACK ? media::engine::VoiceInput::Loopback : media::engine::VoiceInput::Off;
    o.voice_wake = c.wake_policy != MEDIA_WAKE_DISABLED; o.push_to_talk = c.wake_policy == MEDIA_WAKE_DISABLED;
    o.wake_word = c.wake_policy == MEDIA_WAKE_CUSTOM ? e->wake_word : nullptr;
    if (c.speak) { e->speech = {&Speak, e, c.speak_voices}; o.wake_speech = &e->speech; } // Else the system's voices.
    if (c.on_frame) { o.frame_callback = &Forward; o.frame_context = e; }
    o.snapshot_folder = e->snapshot_folder[0] ? e->snapshot_folder : nullptr; // API 8: S and "take snapshot" write there.
    o.parent_window = static_cast<HWND>(e->config.parent_window); // API 12: inside the host's window.
    o.http_headers = e->http_headers[0] ? e->http_headers : nullptr; o.cancel = &e->cancel_open; // API 15.
    e->ran = true; e->stage = 0; e->code = 0;
    if (!e->coordinator.Prepare(e->path, o)) {
        e->stage = e->coordinator.Error() ? e->coordinator.Error() : 1u; e->code = static_cast<std::int32_t>(e->coordinator.NativeError());
        e->coordinator.Close();
        return MEDIA_ERROR_OPEN;
    }
    e->open = true;
    return MEDIA_OK;
}
media_status WindowRun(media_engine* e) noexcept {
    const bool ran = e->coordinator.Run();
    const auto error = e->coordinator.Error();
    if (ran || error == 27) return MEDIA_OK; // 27: maximum_seconds reached, as asked.
    e->stage = error; e->code = static_cast<std::int32_t>(e->coordinator.NativeError());
    return MEDIA_ERROR_PLAYBACK;
}
void WindowStop(media_engine* e) noexcept { e->coordinator.RequestStop(); }
void WindowClose(media_engine* e) noexcept { e->coordinator.Close(); }
media_status WindowCommand(media_engine* e, const char* utf8) noexcept {
    ++e->commands;
    if (e->coordinator.Command(utf8).status == media::ai::IntentStatus::Ok) return MEDIA_OK;
    ++e->refused;
    return MEDIA_ERROR_REFUSED;
}
media_status WindowHear(media_engine* e, const char* utf8) noexcept {
    ++e->commands;
    if (e->coordinator.Hear(utf8)) return MEDIA_OK;
    ++e->refused;
    return MEDIA_ERROR_REFUSED;
}
void WindowSetTalk(media_engine* e, bool pressed) noexcept { e->coordinator.SetTalk(pressed); }
bool WindowSetWake(media_engine* e, media_wake_policy policy) noexcept {
    return e->coordinator.SetWake(policy != MEDIA_WAKE_DISABLED, policy == MEDIA_WAKE_DISABLED, policy == MEDIA_WAKE_CUSTOM ? e->wake_word : nullptr);
}
media_status WindowPoll(media_engine* e, media_frame_signature_t* signature) noexcept {
    FrameSignature s;
    if (!e->coordinator.PollFrame(s)) return MEDIA_ERROR_EMPTY;
    std::memcpy(signature, &s, sizeof(*signature));
    return MEDIA_OK;
}
void WindowStats(const media_engine* e, media_engine_stats_t* stats) noexcept {
    const auto& c = e->coordinator;
    const auto arena = c.ArenaStats();
    stats->arena_capacity_bytes = arena.capacity_bytes; stats->arena_used_bytes = arena.used_bytes;
    stats->module_arena_bytes = c.ModuleArenaBytes();
    stats->threads = c.Threads();
    stats->presented = c.Presented(); stats->dropped = c.Dropped();
    stats->signatures = c.Signatures(); stats->signatures_lost = c.SignaturesLost();
    stats->commands = e->commands; stats->refused = e->refused; stats->wake_ignored = c.WakeIgnored(); stats->wake_templates = c.WakeTemplates();
}
media_status WindowPostCommand(media_engine* e, const char* utf8, std::uint32_t id) noexcept {
    return e->coordinator.PostCommand(utf8, id) ? MEDIA_OK : MEDIA_ERROR_REFUSED;
}
media_status WindowPostControl(media_engine* e, std::uint32_t control, double value, std::uint32_t id) noexcept {
    return e->coordinator.PostControl(static_cast<media::engine::HostControl>(control), value, id) ? MEDIA_OK : MEDIA_ERROR_REFUSED;
}
media_status WindowTrackCount(const media_engine* e, std::uint32_t& count) noexcept { count = e->coordinator.Tracks(); return MEDIA_OK; }
media_status WindowTrackInfo(const media_engine* e, std::uint32_t index, media_track_info_t& out) noexcept {
    const auto& c = e->coordinator;
    if (index >= c.Tracks()) return MEDIA_ERROR_INVALID_ARGUMENT;
    const auto& t = c.TrackAt(index);
    using media::demux::TrackKind;
    const auto size = out.struct_size; std::memset(&out, 0, sizeof(out)); out.struct_size = size;
    out.id = t.id;
    out.kind = t.kind == TrackKind::Video ? MEDIA_TRACK_VIDEO : t.kind == TrackKind::Audio ? MEDIA_TRACK_AUDIO : t.kind == TrackKind::Subtitle ? MEDIA_TRACK_SUBTITLE : MEDIA_TRACK_OTHER;
    if (t.sample_entry) for (int i = 0; i < 4; ++i) { const char ch = static_cast<char>((t.sample_entry >> (24 - 8 * i)) & 0xff); out.codec[i] = ch >= 32 && ch < 127 ? ch : '?'; }
    else if (t.kind == TrackKind::Subtitle && t.id == 0) std::memcpy(out.codec, "srt", 3);
    else media::sdk::CodecText(t.codec, out.codec); // Matroska (API 17): no sample entry, the codec's MP4 name.
    std::memcpy(out.language, t.language, 3); std::memcpy(out.title, t.name, sizeof(out.title) - 1);
    out.channels = t.channels; out.sample_rate = t.sample_rate; out.width = t.width; out.height = t.height;
    out.selectable = t.use == media::engine::TrackUse::Selectable ? 1u : 0u;
    out.is_default = t.enabled ? 1u : 0u;
    if (t.kind == TrackKind::Video) out.active = out.selectable;
    else if (t.kind == TrackKind::Audio) out.active = out.selectable && c.PublishedAudioTrack() == t.id ? 1u : 0u;
    else if (t.kind == TrackKind::Subtitle) out.active = out.selectable && c.PublishedSubtitleTrack() == t.id ? 1u : 0u;
    return MEDIA_OK;
}
media_status WindowPollEvent(media_engine* e, media::engine::HostEvent& event) noexcept {
    return e->coordinator.PollEvent(event) ? MEDIA_OK : MEDIA_ERROR_EMPTY;
}
media_status WindowState(const media_engine* e, media_playback_state_t* out) noexcept {
    const auto s = e->coordinator.PublishedState();
    std::memset(out, 0, sizeof(*out));
    out->position_100ns = s.position; out->duration_100ns = s.duration; out->paused = s.paused ? 1 : 0; out->muted = s.muted ? 1 : 0;
    out->volume = s.volume; out->speed = s.speed; out->presented = s.presented; out->dropped = s.dropped;
    out->view_zoom = 1; out->view_x = out->view_y = 0.5f;
    return MEDIA_OK;
}
media_status WindowOutputTextures(media_engine* e, media_output_textures_t* out) noexcept {
    if (!e->coordinator.TextureOutput()) return MEDIA_ERROR_STATE;
    media::render::D3D12Renderer::OutputDesc d{};
    if (!e->coordinator.OutputTextures(d)) return MEDIA_ERROR_EMPTY;
    FillTextures(d, OutputFormat(e->config.output_format), out);
    return MEDIA_OK;
}
media_status WindowAcquireOutput(media_engine* e, media_output_frame_t* out) noexcept {
    if (!e->coordinator.TextureOutput()) return MEDIA_ERROR_STATE;
    media::render::D3D12Renderer::OutputFrame f{};
    if (!e->coordinator.AcquireOutput(f)) return MEDIA_ERROR_EMPTY;
    FillFrame(f, out);
    return MEDIA_OK;
}
media_status WindowReleaseOutput(media_engine* e, std::uint32_t index, std::uint64_t host_value) noexcept {
    if (!e->coordinator.TextureOutput()) return MEDIA_ERROR_STATE;
    return e->coordinator.ReleaseOutput(index, host_value) ? MEDIA_OK : MEDIA_ERROR_INVALID_ARGUMENT;
}
// API 14: host mode's texture output (HostGpuVideo's shared textures), on the host's threads.
media_status HostOutputTextures(media_engine* e, media_output_textures_t* out) noexcept {
    if (!e->host_gpu.Active()) return MEDIA_ERROR_STATE;
    media::render::SharedOutput::Desc d{};
    if (!e->host_gpu.Output().Describe(d)) return MEDIA_ERROR_EMPTY;
    FillTextures(d, e->host_gpu.Format(), out);
    return MEDIA_OK;
}
media_status HostAcquireOutput(media_engine* e, media_output_frame_t* out) noexcept {
    if (!e->host_gpu.Active()) return MEDIA_ERROR_STATE;
    media::render::SharedOutput::Frame f{};
    if (!e->host_gpu.Output().Acquire(f)) return MEDIA_ERROR_EMPTY;
    FillFrame(f, out);
    return MEDIA_OK;
}
media_status HostReleaseOutput(media_engine* e, std::uint32_t index, std::uint64_t host_value) noexcept {
    if (!e->host_gpu.Active()) return MEDIA_ERROR_STATE;
    return e->host_gpu.Output().ReleaseFrame(index, host_value) ? MEDIA_OK : MEDIA_ERROR_INVALID_ARGUMENT;
}
media_status HostResizeOutput(media_engine* e, std::uint32_t width, std::uint32_t height) noexcept {
    if (!e->host_gpu.Active()) return MEDIA_ERROR_STATE;
    e->host_gpu.RequestSize(width, height); // Applied by the next picture shown (media_engine_step's thread).
    return MEDIA_OK;
}
media_status WindowResizeOutput(media_engine* e, std::uint32_t width, std::uint32_t height) noexcept {
    if (!e->coordinator.TextureOutput()) return MEDIA_ERROR_STATE;
    e->coordinator.ResizeOutput(width, height);
    return MEDIA_OK;
}
media_status GpuOpen(media_engine* e, const media_sensor_config_t& s) noexcept {
    const unsigned width = s.output_width ? s.output_width : s.width, height = s.output_height ? s.output_height : s.height;
    if ((s.output_width == 0) != (s.output_height == 0)) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (!e->gpu.Create(s.width, s.height, width, height, (s.options & MEDIA_SENSOR_GPU_10BIT) != 0))
        return e->gpu.LastError() == E_INVALIDARG ? MEDIA_ERROR_INVALID_ARGUMENT : MEDIA_ERROR_UNSUPPORTED;
    return MEDIA_OK;
}
void GpuIngest(media_engine* e, const media_sensor_frame_t& frame) noexcept {
    if (!e->gpu.Ready()) return;
    const std::uint8_t *y = nullptr, *cb = nullptr, *cr = nullptr; std::uint32_t luma_stride = 0, chroma_stride = 0; std::int64_t pts = 0;
    if (!e->host_engine.SensorPicture(y, cb, cr, luma_stride, chroma_stride, pts)) return;
    // The view shown (with the stabiliser's roll) and how the scene moved since the previous frame (micro-motion follows both).
    media::render::LiveView view;
    e->host_engine.ShownView(view.zoom, view.x, view.y, view.roll);
    const auto& orientation = e->host_engine.OrientationNow();
    orientation.Display(view.display); // API 7: the quarter turns and the mirror.
    view.fit = (e->sensor_options & MEDIA_SENSOR_GPU_FIT) != 0; // API 8: the view whole, bars around it.
    if (const float level = orientation.level; level != 0) {
        // API 6: the leveling turn (the picture turns by it on screen: the view by minus it; through a mirror the stored picture
        // turns the other way), zoomed in to keep the corners covered.
        const double radians = static_cast<double>(level) * 3.14159265358979323846 / 180, aspect = static_cast<double>(frame.width) / frame.height;
        view.roll -= static_cast<float>(orientation.Mirrored() ? -radians : radians);
        view.zoom *= static_cast<float>(std::fabs(std::cos(radians)) + std::fabs(std::sin(radians)) * (std::max)(aspect, 1.0 / aspect));
    }
    e->host_engine.SceneMotion(view.scene_dx, view.scene_dy, view.scene_roll);
    // Busy: refused and counted (the image's `dropped`). A ten-bit ring takes a P010 frame's samples as they are.
    if (e->gpu.TenBit() && frame.format == MEDIA_PIXEL_P010)
        (void)e->gpu.IngestP010(frame.planes[0], frame.strides[0], frame.planes[1], frame.strides[1], pts, view);
    else (void)e->gpu.Ingest(y, luma_stride, cb, cr, chroma_stride, pts, view);
}
void GpuClose(media_engine* e) noexcept { e->gpu.Release(); }
media_status GpuControls(media_engine* e, const media_picture_controls_t& c) noexcept {
    if (!e->gpu.Ready()) return MEDIA_ERROR_UNSUPPORTED;
    e->gpu.SetControls({c.brightness, c.contrast, c.saturation, c.gamma, c.shadow_lift, c.micro_motion, c.reconstruction});
    return MEDIA_OK;
}
media_status GpuImage(media_engine* e, media_gpu_image_t* out) noexcept {
    if (!e->gpu.Ready()) return MEDIA_ERROR_UNSUPPORTED;
    media::render::LiveImage image;
    if (!e->gpu.Latest(image)) return MEDIA_ERROR_EMPTY;
    std::memset(out, 0, sizeof(*out)); out->struct_size = sizeof(*out); // The whole answer; the caller copies its own size's part.
    out->width = image.width; out->height = image.height; out->stride = image.stride; out->rgba = image.rgba;
    out->pts_100ns = image.pts_100ns; out->sequence = image.sequence;
    using media::render::kNeural; using media::render::kEdgeDirected; using media::render::kGrade; using media::render::kMicro;
    out->flags = (image.reconstruction == kNeural ? static_cast<std::uint32_t>(MEDIA_GPU_NEURAL) : 0u) |
                 (image.reconstruction == kEdgeDirected ? static_cast<std::uint32_t>(MEDIA_GPU_EDGE) : 0u) |
                 ((image.picture_flags & kGrade) ? static_cast<std::uint32_t>(MEDIA_GPU_GRADED) : 0u) |
                 ((image.picture_flags & kMicro) ? static_cast<std::uint32_t>(MEDIA_GPU_MICRO_MOTION) : 0u) |
                 (e->host_engine.Stabilizing() ? static_cast<std::uint32_t>(MEDIA_GPU_STABILIZED) : 0u) |
                 (image.ten_bit ? static_cast<std::uint32_t>(MEDIA_GPU_TEN_BIT) : 0u) |
                 ((image.picture_flags & media::render::kMicroWarp) ? static_cast<std::uint32_t>(MEDIA_GPU_MICRO_FOLLOWED) : 0u) |
                 (image.fitted ? static_cast<std::uint32_t>(MEDIA_GPU_FITTED) : 0u);
    out->picture_left = image.picture_left; out->picture_top = image.picture_top; out->picture_right = image.picture_right; out->picture_bottom = image.picture_bottom;
    out->left = image.left; out->top = image.top; out->right = image.right; out->bottom = image.bottom;
    out->gpu_ms = static_cast<float>(image.gpu_ms); out->dropped = e->gpu.Statistics().busy; out->roll = image.roll;
    // API 7: the turns and mirror the image was converted with (a mirror taken out first, as video::Orientation counts them).
    media::video::Orientation shown{}; // Its file matrix (a, b, c, d) whose rows (a c; b d) are the display's.
    const int* d = image.display;
    shown.file[0] = static_cast<std::int8_t>(d[0]); shown.file[1] = static_cast<std::int8_t>(d[2]); shown.file[2] = static_cast<std::int8_t>(d[1]); shown.file[3] = static_cast<std::int8_t>(d[3]);
    out->quarter_turns = shown.QuarterTurns(); out->mirrored = shown.Mirrored() ? 1 : 0;
    return MEDIA_OK;
}
media_status GpuSnapshot(media_engine* e, const char* path, const char* folder, const char* stem, media::video::SnapshotInfo& info,
                         media::video::SnapshotResult& result,
                         const media::video::SnapshotMemory* memory) noexcept {
    if (!e->gpu.Ready()) return MEDIA_ERROR_UNSUPPORTED;
    media::render::LiveImage image;
    if (!e->gpu.Latest(image)) return MEDIA_ERROR_EMPTY;
    // The image exactly as media_query_gpu_image gives it, and how it was made.
    info.source = "gpu-image"; info.pts_100ns = image.pts_100ns;
    info.region[0] = image.left; info.region[1] = image.top; info.region[2] = image.right; info.region[3] = image.bottom;
    info.view_roll_degrees = image.roll * 57.29577951308232f; info.fitted = image.fitted;
    const auto& c = e->gpu.Controls();
    info.brightness = c.brightness; info.contrast = c.contrast; info.saturation = c.saturation; info.gamma = c.gamma; info.shadow_lift = c.shadow_lift;
    info.micro_motion = c.micro_motion;
    info.reconstruction = image.reconstruction == media::render::kNeural ? "neural" : image.reconstruction == media::render::kEdgeDirected ? "edge" : "bilinear";
    const media::video::RgbaImage rgba{image.rgba, image.stride};
    if (memory) return media::video::SaveSnapshotTo(e->encoder, memory, nullptr, folder, stem, image.width, image.height, &media::video::SnapshotEncoder::RgbaRows,
                                                    &rgba, info, result) ? MEDIA_OK : MEDIA_ERROR_STORAGE;
    return media::video::SaveSnapshot(e->encoder, path, folder, stem, image.width, image.height, &media::video::SnapshotEncoder::RgbaRows, &rgba, info, result)
               ? MEDIA_OK : MEDIA_ERROR_OPEN;
}
// Subtitles after open in the window (API 17): PlaybackCoordinator::AttachSubtitles (parsed on this thread, rebound by the window's).
media_status WindowAttachSubtitles(media_engine* e, const char* path) noexcept {
    using R = media::engine::PlaybackCoordinator::AttachResult;
    switch (e->coordinator.AttachSubtitles(path)) {
    case R::Ok: return MEDIA_OK;
    case R::Unsupported: return MEDIA_ERROR_UNSUPPORTED;
    case R::Busy: return MEDIA_ERROR_STATE;
    case R::Unreadable: return MEDIA_ERROR_OPEN;
    default: return MEDIA_ERROR_PLAYBACK;
    }
}
media_status WindowSnapshotMemory(media_engine* e, const media::video::SnapshotMemory& memory, media::video::SnapshotResult& result) noexcept {
    // Asked of the window's thread (it owns the renderer's snapshot state) and waited for here, at most 3 s.
    auto& c = e->coordinator;
    if (!c.RequestMemorySnapshot(memory)) return MEDIA_ERROR_STATE; // One on its way, or called from the window's own thread.
    const auto deadline = GetTickCount64() + 3000;
    while ((c.MemorySnapshotState() == 1 || c.MemorySnapshotState() == 2) && GetTickCount64() < deadline) Sleep(1);
    const int state = c.MemorySnapshotState();
    if (state == 1 || state == 2) return MEDIA_ERROR_PLAYBACK; // Not made in time.
    media::engine::PlaybackCoordinator::SnapshotRecord record{};
    const bool known = c.LastSnapshot(record);
    c.ResetMemorySnapshot();
    result = {}; result.written = known && record.written; result.width = record.width; result.height = record.height; result.pts_100ns = record.pts_100ns;
    result.hash = record.hash;
    if (state == 4 && !record.width) return MEDIA_ERROR_EMPTY;
    return state == 3 ? MEDIA_OK : MEDIA_ERROR_STORAGE;
}
media_status WindowSnapshot(media_engine* e, const char* path) noexcept {
    switch (e->coordinator.Snapshot(path)) {
    case media::ai::IntentStatus::Ok: return MEDIA_OK;
    case media::ai::IntentStatus::NoContext: return MEDIA_ERROR_STATE;   // One is being written.
    case media::ai::IntentStatus::NotFound: return MEDIA_ERROR_EMPTY;    // No picture yet.
    case media::ai::IntentStatus::Malformed: return MEDIA_ERROR_INVALID_ARGUMENT;
    default: return MEDIA_ERROR_UNSUPPORTED;
    }
}
media_status WindowEventCamera(media_engine* e, const media::video::EventCameraOptions* options) noexcept {
    return e->coordinator.SetEventCamera(options) ? MEDIA_OK : MEDIA_ERROR_UNSUPPORTED;
}
media_status WindowPollEvents(media_engine* e, media::video::EventSpike* spikes, std::uint32_t capacity, std::uint32_t& count) noexcept {
    if (!e->coordinator.EventCamera().configured) return MEDIA_ERROR_UNSUPPORTED;
    count = e->coordinator.PollEvents(spikes, capacity);
    return count ? MEDIA_OK : MEDIA_ERROR_EMPTY;
}
media_status WindowEventState(const media_engine* e, media_event_camera_state_t* state) noexcept {
    const auto c = e->coordinator.EventCamera();
    if (!c.configured) return MEDIA_ERROR_UNSUPPORTED;
    std::memset(state, 0, sizeof(*state)); state->struct_size = sizeof(*state);
    state->on = c.on ? 1 : 0; state->width = c.width; state->height = c.height; state->scale = c.scale;
    state->pictures = c.pictures; state->events = c.events; state->dropped = c.dropped; state->rebases = c.rebases; state->last_events = c.last;
    return MEDIA_OK;
}
media_status WindowTensor(media_engine* e, const media::video::TensorSpec& spec, const media_tensor_request_t& r, void* tensor, std::size_t bytes,
                          media_tensor_info_t* info) noexcept {
    using media::render::D3D12Renderer;
    const std::uint32_t kind = r.region == MEDIA_TENSOR_VIEW ? D3D12Renderer::kTensorView : r.region == MEDIA_TENSOR_FULL ? D3D12Renderer::kTensorFull :
                               r.region == MEDIA_TENSOR_REGION ? D3D12Renderer::kTensorCustom : D3D12Renderer::kTensorFovea;
    const float fractions[4]{r.left, r.top, r.right, r.bottom};
    D3D12Renderer::TensorAnswer answer;
    const int got = e->coordinator.ExportTensor(spec, kind, fractions, tensor, bytes, answer, 1000);
    if (got == 0) return MEDIA_ERROR_EMPTY;
    if (got < 0) return MEDIA_ERROR_INVALID_ARGUMENT;
    if (info) {
        std::memset(info, 0, sizeof(*info)); info->struct_size = sizeof(*info);
        info->region = r.region; info->left = static_cast<float>(answer.region.left); info->top = static_cast<float>(answer.region.top);
        info->right = static_cast<float>(answer.region.right); info->bottom = static_cast<float>(answer.region.bottom);
        info->pts_100ns = answer.pts; info->bytes = media::video::TensorBytes(spec); info->target_track_id = kind == D3D12Renderer::kTensorFovea ? answer.target_id : 0u;
        info->flags = (media::video::IsHdrTensor(answer.color) ? static_cast<std::uint32_t>(MEDIA_TENSOR_TONE_MAPPED) : 0u) | (answer.held ? static_cast<std::uint32_t>(MEDIA_TENSOR_HELD) : 0u);
    }
    return MEDIA_OK;
}
media_status WindowStabilization(media_engine* e, bool on) noexcept {
    return e->coordinator.SetStabilization(on) ? MEDIA_OK : MEDIA_ERROR_UNSUPPORTED;
}
media_status WindowSnapshotQuery(const media_engine* e, media_snapshot_t* out) noexcept {
    media::engine::PlaybackCoordinator::SnapshotRecord r;
    if (!e->coordinator.LastSnapshot(r)) return MEDIA_ERROR_EMPTY;
    std::memset(out, 0, sizeof(*out)); out->struct_size = sizeof(*out);
    out->sequence = r.sequence; out->status = r.written ? MEDIA_OK : MEDIA_ERROR_OPEN; out->source = MEDIA_SNAPSHOT_WINDOW;
    out->width = r.width; out->height = r.height; out->pts_100ns = r.pts_100ns; out->frame_hash = r.hash;
    std::memcpy(out->path, r.path, sizeof(out->path)); out->path[sizeof(out->path) - 1] = 0;
    return MEDIA_OK;
}
void WindowSnapshotFolder(media_engine* e) noexcept { e->coordinator.SetSnapshotFolder(e->snapshot_folder[0] ? e->snapshot_folder : nullptr); }
}
