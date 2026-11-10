#pragma once
// The C SDK's engine handle (docs/sdk.md), shared by the portable part (CApi.cpp: configuration, budgets, host mode) and the
// windowed part (CApiEngine.cpp, Windows). It lives in the host's storage; nothing in it allocates.
#include "media/c_api.h"
#include "media/core/spsc_ring_buffer.hpp"
#include "media/engine/HostEngine.hpp"
#include "media/engine/HostEvents.hpp"
#include "media/video/Snapshot.hpp"
#include "media/video/TensorExport.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#if defined(_WIN32)
#include "media/engine/PlaybackCoordinator.hpp"
#include "media/platform/windows/SapiSpeech.hpp"
#include "media/render/LiveGpuPipeline.hpp"
#include "HostGpu.hpp"
#endif

struct media_engine {
    media_engine_config_t config{};
    char wake_word[32]{};
    char subtitles[1024]{};          // The sidecar path (UTF-8): the host's string need not outlive create.
    bool open = false, ran = false, host = false;
    std::uint32_t commands = 0, refused = 0, stage = 0;
    std::uint32_t sensor_options = 0;   // media_sensor_config_t.options of the open sensor (MEDIA_SENSOR_GPU_FIT: GpuIngest's view).
    // Forensic snapshots (API 8; docs/sdk.md, "Snapshots"): host mode writes them on the host's thread with this encoder; the
    // windowed engine's are its coordinator's (its snapshot thread).
    char media[1024]{};                 // Host mode: the file opened (UTF-8; empty: a sensor), for the names and the sidecar.
    media::video::SnapshotResult last_memory_{}; // API 17: host mode's last snapshot into memory.
    char snapshot_folder[512]{};        // media_engine_set_snapshot_folder (UTF-8; empty: the default).
    media_snapshot_t snapshot{};        // Host mode: the last one (struct_size 0: none yet).
    std::uint32_t snapshots = 0;        // Host mode: written since open.
    media::video::SnapshotEncoder encoder;
    std::int32_t code = 0;
    // API 15 (media_engine_last_error_text): after a failed open, why, from media_probe on the same file (probe_scratch: its
    // working struct, kept here so that no open needs a large stack frame).
    bool open_failed = false;
    char failed_reason[160]{};
    media_probe_t probe_scratch{};
    // API 15: media_engine_open_ex's headers (wide, for WinHTTP) and media_engine_cancel_open's flag, which outlives the
    // coordinator (WindowOpen rebuilds it). `opening`: an open is in progress (a cancel outside one does nothing).
    wchar_t http_headers[4096]{};
    std::atomic<bool> opening{false}, cancel_open{false};
    media::ai::SpeechSource speech{}; // The host's speak callback as the engine's speech source.
#if defined(_WIN32)
    // API 14: host mode's hardware decoding into texture output. Declared before host_engine, which uses it until it is destroyed.
    media::sdk::HostGpuVideo host_gpu;
#endif
    media::engine::HostEngine host_engine;
    // Embedding (API 12) in host mode: commands posted by the host's UI thread, run by the next media_engine_step, each answered
    // by an event. The windowed engine's are its coordinator's (PostCommand, PollEvent).
    media::core::SpscRingBuffer<media::engine::HostCommand, 32> host_commands;
    media::core::SpscRingBuffer<media::engine::HostEvent, 64> host_events;
    std::uint32_t host_events_lost = 0;
#if defined(_WIN32)
    wchar_t path[2048]{}, subtitles_wide[1024]{};
    media::engine::PlaybackCoordinator coordinator;
    media::platform::windows::SapiSpeech system; // Host mode on Windows: the system's voices for a custom name.
    media::render::LiveGpuPipeline gpu;           // Live sensors with MEDIA_SENSOR_GPU (docs/sdk.md, "GPU live ingest").
    media_engine() noexcept : coordinator(nullptr) {}
#else
    media_engine() noexcept = default;
#endif
};

namespace media::sdk {
// The windowed engine (MEDIA_PLAYBACK_WINDOW): Windows only; elsewhere MEDIA_ERROR_UNSUPPORTED (CApi.cpp).
media_status WindowOpen(media_engine* e, const char* path) noexcept;
media_status WindowRun(media_engine* e) noexcept;
void WindowStop(media_engine* e) noexcept;
void WindowClose(media_engine* e) noexcept;
media_status WindowCommand(media_engine* e, const char* utf8) noexcept;
media_status WindowHear(media_engine* e, const char* utf8) noexcept;
void WindowSetTalk(media_engine* e, bool pressed) noexcept;
bool WindowSetWake(media_engine* e, media_wake_policy policy) noexcept;
media_status WindowPoll(media_engine* e, media_frame_signature_t* signature) noexcept;
void WindowStats(const media_engine* e, media_engine_stats_t* stats) noexcept;
// Embedding (API 12): the coordinator's rings and published state (MEDIA_ERROR_UNSUPPORTED elsewhere).
media_status WindowPostCommand(media_engine* e, const char* utf8, std::uint32_t id) noexcept;
media_status WindowPostControl(media_engine* e, std::uint32_t control, double value, std::uint32_t id) noexcept;
media_status WindowTrackCount(const media_engine* e, std::uint32_t& count) noexcept;
media_status WindowTrackInfo(const media_engine* e, std::uint32_t index, media_track_info_t& info) noexcept;
media_status WindowPollEvent(media_engine* e, media::engine::HostEvent& event) noexcept;
media_status WindowState(const media_engine* e, media_playback_state_t* state) noexcept;
// Texture output (API 13): the renderer's shared textures (MEDIA_ERROR_UNSUPPORTED elsewhere).
media_status WindowOutputTextures(media_engine* e, media_output_textures_t* out) noexcept;
media_status WindowAcquireOutput(media_engine* e, media_output_frame_t* out) noexcept;
media_status WindowReleaseOutput(media_engine* e, std::uint32_t index, std::uint64_t host_value) noexcept;
media_status WindowResizeOutput(media_engine* e, std::uint32_t width, std::uint32_t height) noexcept;
// API 14: host mode's texture output, from its hardware decoding (Windows; MEDIA_ERROR_STATE when the GPU did not take the stream,
// MEDIA_ERROR_UNSUPPORTED elsewhere).
media_status HostOutputTextures(media_engine* e, media_output_textures_t* out) noexcept;
media_status HostAcquireOutput(media_engine* e, media_output_frame_t* out) noexcept;
media_status HostReleaseOutput(media_engine* e, std::uint32_t index, std::uint64_t host_value) noexcept;
media_status HostResizeOutput(media_engine* e, std::uint32_t width, std::uint32_t height) noexcept;
// GPU live ingest (MEDIA_SENSOR_GPU): Windows only; elsewhere MEDIA_ERROR_UNSUPPORTED.
media_status GpuOpen(media_engine* e, const media_sensor_config_t& sensor) noexcept;
void GpuIngest(media_engine* e, const media_sensor_frame_t& frame) noexcept; // After a camera frame was ingested by the host engine.
void GpuClose(media_engine* e) noexcept;
media_status GpuControls(media_engine* e, const media_picture_controls_t& controls) noexcept;
media_status GpuImage(media_engine* e, media_gpu_image_t* image) noexcept;
// Snapshots (API 8). Host mode with the GPU path: the newest GPU image written (MEDIA_ERROR_UNSUPPORTED without it, so the
// software view is written instead). The windowed engine: asked of the coordinator (written by its thread), its last one, its folder.
media_status GpuSnapshot(media_engine* e, const char* path, const char* folder, const char* stem, media::video::SnapshotInfo& info,
                         media::video::SnapshotResult& result, const media::video::SnapshotMemory* memory = nullptr) noexcept;
media_status WindowSnapshot(media_engine* e, const char* path) noexcept;
// API 17: subtitles after open in the window (PlaybackCoordinator::AttachSubtitles).
media_status WindowAttachSubtitles(media_engine* e, const char* path) noexcept;
// API 17: the window's snapshot into the host's buffer, waited for (at most 3 s): its size and hash in `result`.
media_status WindowSnapshotMemory(media_engine* e, const media::video::SnapshotMemory& memory, media::video::SnapshotResult& result) noexcept;
media_status WindowSnapshotQuery(const media_engine* e, media_snapshot_t* snapshot) noexcept;
void WindowSnapshotFolder(media_engine* e) noexcept;
// The event camera and tensor export in the window (API 10): the coordinator's renderer's (D3D12Renderer::SetEventCamera,
// RequestTensor); a tensor waits at most a second for the next picture converted.
media_status WindowEventCamera(media_engine* e, const media::video::EventCameraOptions* options) noexcept;
media_status WindowStabilization(media_engine* e, bool on) noexcept; // API 11: the window's view (D3D12Renderer::SetStabilization).
media_status WindowPollEvents(media_engine* e, media::video::EventSpike* spikes, std::uint32_t capacity, std::uint32_t& count) noexcept;
media_status WindowEventState(const media_engine* e, media_event_camera_state_t* state) noexcept;
media_status WindowTensor(media_engine* e, const media::video::TensorSpec& spec, const media_tensor_request_t& request, void* tensor, std::size_t bytes,
                          media_tensor_info_t* info) noexcept;
// A track's codec as the SDK names it (media_track_info_t::codec, API 17): the MP4 sample entry's fourcc for the codecs the engine
// knows, so that a Matroska track reads as its MP4 twin ("avc1", "hvc1", "av01", "mp4a", "Opus", "tx3g"; "text" and "ssa" for
// Matroska's text subtitles); "" for any other.
inline void CodecText(media::demux::Codec codec, char (&out)[8]) noexcept {
    using media::demux::Codec;
    const char* name = codec == Codec::H264 ? "avc1" : codec == Codec::Hevc ? "hvc1" : codec == Codec::Av1 ? "av01" : codec == Codec::Aac ? "mp4a" :
                       codec == Codec::Opus ? "Opus" : codec == Codec::Tx3g ? "tx3g" : codec == Codec::Pcm ? "ipcm" : codec == Codec::Text ? "text" :
                       codec == Codec::Ssa ? "ssa" : "";
    unsigned i = 0; for (; name[i] && i < 7; ++i) out[i] = name[i];
    out[i] = 0;
}
}