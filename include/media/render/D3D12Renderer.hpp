#pragma once
#include "media/platform/windows/D3D12VideoDecoder.hpp"
#include "media/clock/PredictiveMasterClock.hpp"
#include "media/video/HorizonLevel.hpp"
#include "media/video/Matte.hpp"
#include "media/video/Orientation.hpp"
#include "media/video/ReorderWindow.hpp"
#include "media/video/PictureGeometry.hpp"
#include "media/render/ColorPipeline.hpp"
#include "media/render/HudPipeline.hpp"
#include "media/render/VisualPipeline.hpp"
#include "media/audio/SpectrumAnalyser.hpp"
#include "media/render/SdfSubtitlePipeline.hpp"
#include "media/render/NeuralUpscalePass.hpp"
#include "media/render/PalettePipeline.hpp"
#include "media/render/StatusPipeline.hpp"
#include "media/render/QualityGovernor.hpp"
#include "media/render/PictureBlit.hpp"
#include "media/render/SharedOutput.hpp"
#include "media/timeline/ChapterTimeline.hpp"
#include "media/video/TargetTracker.hpp"
#include "media/video/Perception.hpp"
#include "media/video/EventCamera.hpp"
#include "media/video/Vibrometry.hpp"
#include "media/video/VirtualImu.hpp"
#include "media/video/Stabilizer.hpp"
#include "media/video/TensorExport.hpp"
#include "media/core/spsc_ring_buffer.hpp"
#include <atomic>
#include <dxgi1_6.h>
#include <string_view>
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Embedded cache-aligned transport descriptor.
#endif
namespace media::render {
enum class RenderStatus { Ready, WouldBlock, Closed, Failed, DeviceLost };
struct PresentState {
    bool draining = false; // Input ended: frames below the reorder depth may leave the window.
    bool paused = false;   // Hold presentation of new frames.
};
// A window passed from one file's renderer to the next (docs/session.md, "Next and previous file"): the next file plays
// in the same window, at the same place and size, fullscreen or not, instead of one window closing and another opening.
struct WindowHandoff { HWND window = nullptr; bool fullscreen = false; RECT restore{}; };
struct CompositeContext {
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* commands = nullptr;
    std::uint64_t fence = 0;
};
// What a warm session keeps from one file to the next (docs/session.md, "Warm switch"): the window and its swap chain,
// the device, the direct queue and its fence (whose values go on rising), the composition lists, and the passes that do
// not depend on the video (colour conversion, dock, picture blit, status cue, command bar, with the host memory of the
// last two). A renderer constructed with a session works on these objects in place; the first one creates them, and a
// later one finds them created and makes only what its file needs (slots, descriptors, the neural pass, subtitles). A
// device loss replaces them as for any renderer. Close releases the file's objects and leaves these. Close() here
// releases everything (the session's end), after the last renderer has closed.
struct GpuSession {
    GpuSession() noexcept = default;
    GpuSession(const GpuSession&) = delete;
    GpuSession& operator=(const GpuSession&) = delete;
    ~GpuSession() noexcept { Close(); }
    void Close() noexcept;
    bool Live() const noexcept { return device != nullptr; }
    WindowHandoff window{};
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    std::uint64_t sequence = 0;
    HANDLE latency = nullptr, event = nullptr;
    IDXGISwapChain3* swap = nullptr;
    ID3D12Resource* buffers[3]{};
    ID3D12DescriptorHeap* rtvs = nullptr;
    unsigned rtv_stride = 0, width = 0, height = 0; // The swap chain's buffers.
    CompositeContext composites[3]{};
    ID3D12RootSignature* blit_root = nullptr;
    ID3D12PipelineState* blit = nullptr;
    ColorPipeline color;
    HudPipeline hud;
    StatusPipeline status;
    PalettePipeline palette;
    VisualPipeline visual;
    core::LinearMemoryArena arena; // Status cue and command bar text buffers (4 MiB, created with them).
    // Between files: the RGB slots (with their lists) and the history, which the next file takes when its pictures are
    // fitted to the same size (the slots are sized for the video's shape on this desktop). Every list leaves them COMMON.
    struct KeptSlot { ID3D12Resource* rgb = nullptr; ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* commands = nullptr; };
    KeptSlot slots[16]{};
    ID3D12Resource* history = nullptr;
    unsigned slot_width = 0, slot_height = 0;
    void ReleaseSlots() noexcept;
    std::uint32_t status_revision = 0, palette_revision = 0; // The models' revisions go on from file to file.
};
// Presentation owner (UI thread). Decoded NV12/P010 surfaces are converted into display-sized RGB slots
// immediately and returned to the decoder, so presentation timing never holds decode surfaces. The RGB
// slots wait in a ReorderWindow and leave in PTS order at their clock deadlines.
class D3D12Renderer final {
public:
    static constexpr unsigned Count = 16; // RGB slots; admits reorder depths up to Count - 2.
    static_assert(sizeof(GpuSession::slots) / sizeof(GpuSession::slots[0]) == Count);
    // `session`: a warm session's objects (GpuSession), used and kept in place; nullptr: the renderer's own.
    explicit D3D12Renderer(GpuSession* session = nullptr) noexcept : gpu_(session ? *session : own_), session_(session) {}
    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;
    ~D3D12Renderer() noexcept;
    // `activate` false shows the window without taking keyboard focus (automated runs ignore stray keystrokes).
    // `video_width/height` (the stream's crop in samples, 0 if unknown) and `sample_aspect` (the shape of one sample)
    // give the picture's display shape, video::DisplaySize. The RGB slots are sized for the largest picture of that
    // shape the desktop can show, so later resizes and fullscreen need no GPU allocation; an unknown size sizes them
    // to the initial window. Every picture is converted and letterboxed at that shape: anamorphic video is shown
    // undistorted.
    bool Prepare(unsigned width, unsigned height, unsigned surfaces, bool visible = true, unsigned reorder_depth = 0, bool activate = true,
                 unsigned video_width = 0, unsigned video_height = 0, video::SampleAspect sample_aspect = {}) noexcept;
    video::SampleAspect SampleAspect() const noexcept { return sample_aspect_; }
    bool Bind(const platform::windows::D3D12VideoDecoder&) noexcept; // NV12 or P010 decode surfaces.
    using HudHandler = bool (*)(void*, ui::HudAction) noexcept;
    using KeyHandler = void (*)(void*, unsigned) noexcept;
    using PumpHandler = void (*)(void*) noexcept;
    // Called from WM_TIMER while Windows runs its modal move/size loop, so video keeps streaming during a drag.
    void ConfigurePump(PumpHandler handler, void* context) noexcept { pump_ = handler; pump_context_ = context; }
    void ConfigureKeys(KeyHandler handler, void* context) noexcept { key_handler_ = handler; key_context_ = context; }
    void InputKey(unsigned key) noexcept { if (key_handler_) key_handler_(key_context_, key); }
    void ConfigureHud(std::int64_t duration, bool always_visible, HudHandler, void* context) noexcept;
    void InputHud(const ui::PointerEvent&) noexcept; // Same UI-owner entry point as WindowProc.
    // `min_cells` (API 17): an atlas of at least that many cells and an empty track accepted (ready for subtitles after open).
    bool PrepareSubtitles(core::LinearMemoryArena& arena, const subtitle::SubtitleTrack& track, unsigned min_cells = 0) noexcept;
    // Subtitles after open (API 17), any thread: the presentation thread rebinds the pipeline to `track` on its next pass
    // (SdfSubtitlePipeline::Rebind). False when the subtitles were not prepared or a rebind is still asked for.
    bool RequestSubtitleRebind(const subtitle::SubtitleTrack* track) noexcept;
    // Whether `track` has been taken: asked for no more, and bound by the shaping thread.
    bool SubtitleRebound(const subtitle::SubtitleTrack* track) const noexcept {
        return !pending_rebind_.load(std::memory_order_acquire) && subtitles_.Bound() == track;
    }
    unsigned SubtitlePresents() const noexcept { return subtitle_presents_; }
    unsigned CuesShown() const noexcept { return cues_shown_; } // Distinct cues composited at least once.
    // Cues composited into one presented image, selected at `time`: the PTS of the video frame in that image, or
    // for a controls-only refresh the media position of the image already on screen.
    struct SubtitleFrame { std::int64_t time = -1; unsigned count = 0; unsigned cues[subtitle::SubtitleTrack::kMaxActive]{}; };
    const SubtitleFrame& ShownSubtitles() const noexcept { return shown_subtitles_; }
    const SdfSubtitlePipeline& Subtitles() const noexcept { return subtitles_; }
    // Window geometry (UI thread). A new client size is applied to the swap chain once in-flight composites retire;
    // until then presentation waits (WouldBlock). Fullscreen is borderless on the window's monitor (F11, Alt+Enter).
    void ToggleFullscreen() noexcept;
    std::int64_t RefreshPeriod() const noexcept { return refresh_period_; } // The desktop compositor's, in 100 ns.
    bool Fullscreen() const noexcept { return fullscreen_; }
    unsigned Width() const noexcept { return width_; }   // Current back-buffer size.
    unsigned Height() const noexcept { return height_; }
    float DpiScale() const noexcept { return dpi_; }     // Window DPI / 96.
    unsigned Resizes() const noexcept { return resizes_; } // Swap-chain resizes applied.
    bool ResizePending() const noexcept { return resize_pending_; }
    struct VideoRect { unsigned x = 0, y = 0, width = 0, height = 0; };
    const VideoRect& LastVideoRect() const noexcept { return video_rect_; } // Letterboxed video in the last composite.
    // Distance from the bottom edge to the lowest caption: 1.3 font sizes at rest (40 px at 720p); while the dock is
    // shown, 20 scaled px above its top edge. Glides with the dock's visibility.
    static float CaptionInset(const ui::HudUniforms&, unsigned width, unsigned height, float dpi_scale) noexcept;
    unsigned ScaledComposites() const noexcept { return scaled_composites_; } // Pictures stretched after a resize.
    unsigned SlotWidth() const noexcept { return capacity_width_; }  // RGB slot capacity.
    unsigned SlotHeight() const noexcept { return capacity_height_; }
    void SetHudPaused(bool paused) noexcept { hud_paused_ = paused; }
    // Scrub preview: the picture on screen is a keyframe preview (the dock shows it until an exact landing). UI thread.
    void SetHudPreview(bool preview) noexcept { hud_.SetPreview(preview); redraw_ = true; }
    // Chapter and scene ticks on the dock's timeline, drawn from the next composite (UI thread, any time).
    void SetTimelineMarks(const ui::TimelineMarks& marks) noexcept { marks_ = marks; redraw_ = true; }
    // The A–B loop's bracket on the dock's timeline (timeline::LoopRegion::Packed; 0: none). UI thread, any time.
    void SetLoop(std::uint32_t packed) noexcept { hud_.SetLoop(packed, InputTime()); redraw_ = true; }
    const ui::TimelineMarks& Marks() const noexcept { return marks_; }
    void SetBookmarkMarks(const ui::BookmarkMarks& marks) noexcept { bookmark_marks_ = marks; hud_.Reveal(InputTime()); redraw_ = true; }
    // The engine's output volume and mute, for the dock's volume pill (UI thread, as often as convenient).
    // Music mode draws the sound as it is heard: through the same gain (audio::OutputGain), flat and dimmed while muted.
    void SetVolume(float volume, bool muted) noexcept { hud_.SetVolume(volume, muted, InputTime()); volume_ = volume; muted_ = muted; }
    unsigned VolumePresents() const noexcept { return volume_presents_; } // Composites that drew the volume pill.
    // Command bar (ui::CommandPalette drawn by PalettePipeline): a floating text field for commands and quotes.
    // - `/`, Ctrl+F or Ctrl+K opens it; while it is open every key types into it and no playback shortcut fires.
    // - Text arrives as WM_CHAR / WM_UNICHAR code units and, from an input method, as WM_IME_COMPOSITION strings
    //   (the composition is drawn in the bar, underlined; the candidate window is placed under the caret). The
    //   window has no input method attached while the bar is closed, so shortcuts work under any keyboard layout.
    // - Enter gives the text to the submit handler: true closes the bar, false keeps it open and shakes it.
    //   Escape, or a click outside the bar, closes it. Backspace deletes a character, Ctrl+Backspace a word,
    //   Ctrl+V pastes.
    // - The preview handler says what Enter would do with the text as typed so far (shown as the bar's icon and
    //   match counter).
    // PreparePalette (Preparing only) takes the text buffers from the arena; without it the bar never opens.
    using PaletteSubmit = bool (*)(void*, std::string_view utf8) noexcept;
    using PalettePreview = ui::PaletteHint (*)(void*, std::string_view utf8, unsigned& matches) noexcept;
    bool PreparePalette(core::LinearMemoryArena&) noexcept;
    void ConfigurePalette(PaletteSubmit submit, PalettePreview preview, void* context) noexcept {
        palette_submit_ = submit; palette_preview_ = preview; palette_context_ = context;
    }
    bool OpenPalette() noexcept;
    void ClosePalette() noexcept;
    const ui::CommandPalette& Palette() const noexcept { return palette_; }
    const PalettePipeline& PaletteText() const noexcept { return palette_pipeline_; }
    unsigned PalettePresents() const noexcept { return palette_presents_; }     // Composites that drew the bar.
    // Status cue (ui::StatusCue drawn by StatusPipeline, docs/kinetic-hud.md): a transient glass pill at the top centre.
    // PrepareStatus (Preparing only) starts its text thread; without it ShowStatus does nothing. UI thread.
    bool PrepareStatus(core::LinearMemoryArena&) noexcept;
    void ShowStatus(std::string_view utf8, ui::StatusIcon, ui::StatusTone, float hold_seconds = ui::StatusCue::kHoldSeconds) noexcept;
    const ui::StatusCue& Status() const noexcept { return status_; }
    const StatusPipeline& StatusText() const noexcept { return status_pipeline_; }
    unsigned StatusPresents() const noexcept { return status_presents_; } // Composites that drew the pill.
    unsigned StatusShown() const noexcept { return status_shown_; }       // Cues that reached the screen with their text.
    unsigned PaletteSubmits() const noexcept { return palette_submits_; }       // Enter presses with text.
    unsigned PaletteRejections() const noexcept { return palette_rejections_; } // Of those, refused by the handler.
    // Media time of the image on screen: the clock while playing, else the presented frame (a seek's landing frame
    // stands for the seek target inside its interval).
    std::int64_t MediaPosition() const noexcept { return hud_media_; }
    unsigned HudPresents() const noexcept { return hud_presents_; }
    unsigned PausedHudPresents() const noexcept { return paused_hud_presents_; }
    // Music mode (docs/audio-dsp.md, "Music mode"): a file without pictures shows its sound (VisualPipeline) as the
    // background of the controls, composed at the display's rate while playing and held, dimmed, while paused. ShowAudio
    // hands in the picture of the block heard now (the caller times it by the clock): the bars and the level rise to it
    // at once and fall at most 1.6 of their range a second. LandAudio stands for a seek's landing picture: the epoch's
    // sound is playing (its clock anchored at `media`). Set before Prepare. UI thread.
    void SetMusicMode(bool on) noexcept { music_ = on; }
    bool MusicMode() const noexcept { return music_; }
    void ShowAudio(const audio::AudioPicture& picture) noexcept { audio_picture_ = picture; ++audio_pictures_; }
    void LandAudio(std::int64_t media) noexcept;
    unsigned MusicPresents() const noexcept { return music_presents_; }  // Composites that drew the sound.
    unsigned AudioPictures() const noexcept { return audio_pictures_; }  // Pictures handed in.
    const float* ShownBands() const noexcept { return shown_bands_; }   // As last drawn, 0..1 of 0..255 (0 dB: 235 / 255).
    float ShownLevel() const noexcept { return shown_level_; }
    unsigned RejectedSeeks() const noexcept { return rejected_seeks_; }
    const ui::HudUniforms& HudState() const noexcept { return hud_.Uniforms(); }
    ID3D12Device* Device() const noexcept { return device_; }
    ID3D12Fence* ConsumerFence() const noexcept { return fence_; }
    HWND Window() const noexcept { return window_; } // Borrowed, presentation-owner thread only.
    bool Messages() noexcept;
    // The window thread (docs/session.md, "The window thread", 2026-10-28). With SetMessageThread(true) before Prepare (the
    // playback engine's default) the window is made by, and its messages arrive on, a thread of its own (one for the process,
    // which keeps every engine window, so a window handed from file to file keeps its thread). Its procedure answers at once
    // what must be answered at once (hit testing, the least size, activation, the input method's context), does what only the
    // window's thread may (mouse capture and tracking, reading the input method's strings, placing its windows, destroying the
    // window), and passes everything else, with the modifier keys as they were, through a single-producer single-consumer
    // ring (InputEvent, pre-allocated) to the presentation thread, which handles it in Messages() exactly as the procedure did.
    // A stalled message dispatch (the desktop, the compositor, a slow procedure) then stalls only the window thread: pictures
    // keep converting and flipping; and input wakes the presentation thread at once (InputWake()) instead of waiting behind a
    // due picture. Without it (unit tests that drive the window procedure themselves) the window is this thread's, as before.
    void SetMessageThread(bool on) noexcept { message_thread_ = on; }
    // The warm grids (perception every kWarmInterval while not tracking, so that T and Tab find the objects already known).
    // False: none (PlaybackOptions::warm_perception); tracking, events and stabilisation still grid when asked for.
    void SetWarmPerception(bool on) noexcept { warm_perception_ = on; }
    bool MessageThread() const noexcept { return message_thread_; }
    // Embedding (API 12; docs/sdk.md, "Embedding: API 12"), before Prepare: the window is a borderless child of `parent` filling its
    // client area and following its size (a WinEvent hook on the window's thread: no polling); F, F11 and Alt+Enter are
    // counted for the host (FullscreenRequests) instead of filling the monitor, Escape no longer closes, and the parent's
    // destruction closes (Closed). Null: the engine's own top-level window.
    void SetParentWindow(HWND parent) noexcept { parent_ = parent; }
    HWND ParentWindow() const noexcept { return parent_; }
    std::uint32_t FullscreenRequests() const noexcept { return fullscreen_requests_.load(std::memory_order_relaxed); }
    bool Closed() const noexcept { return closed_.load(std::memory_order_relaxed); }
    // Texture output (API 13; docs/sdk.md, "Texture output: API 13"), before Prepare: no swap chain. Each composite is drawn into
    // one of three shared textures (render::SharedOutput: NT handles, D3D12_HEAP_FLAG_SHARED) of `width` x `height`, then a shared
    // fence is signalled with its number and the texture published; a host's renderer (D3D11, D3D12) opens them once, acquires the
    // newest frame, waits for its fence value on its GPU, and releases it with a value of its own on the host fence, which the
    // engine's queue waits for (on the GPU) before drawing into that texture again. Lock-free both ways; nothing waits on the CPU.
    // API 14: `format` 8-bit (R8G8B8A8, the default), 10-bit PQ BT.2020 or FP16 scRGB: the slots and the composites are then of
    // that format, the conversion writes it (HDR kept, SDR at reference white: ColorPipeline kHdrOutput), and the engine's
    // overlays (captions, the dock, cues, the command bar, music mode's visual) and snapshots are not drawn (the host draws its own).
    void SetTextureOutput(bool on, unsigned width, unsigned height, SharedOutput::Format format = SharedOutput::Format::Rgba8) noexcept {
        texture_output_ = on; output_width_ = width; output_height_ = height; output_format_ = on ? format : SharedOutput::Format::Rgba8;
    }
    bool TextureOutput() const noexcept { return texture_output_; }
    bool HdrOutput() const noexcept { return texture_output_ && output_format_ != SharedOutput::Format::Rgba8; }
    static constexpr unsigned kOutputs = SharedOutput::kCount;
    using OutputDesc = SharedOutput::Desc;
    using OutputFrame = SharedOutput::Frame;
    // Any one host thread: the textures (valid until the generation changes: a resize), the newest frame (false: none newer than the
    // one held), its release, and a size to draw at from the next composite.
    bool OutputTextures(OutputDesc& out) const noexcept { return output_.Describe(out); }
    bool AcquireOutput(OutputFrame& out) noexcept { return output_.Acquire(out); }
    bool ReleaseOutput(unsigned index, std::uint64_t host_value) noexcept { return output_.ReleaseFrame(index, host_value); }
    void RequestOutputSize(unsigned width, unsigned height) noexcept { output_request_.store(std::uint64_t{width} << 32 | height, std::memory_order_release); }
    void PollOutputSize() noexcept; // The presentation thread: a size the host asked for becomes a resize.
    std::uint32_t OutputDropped() const noexcept { return output_.Dropped(); }
    HANDLE InputWake() const noexcept { return input_wake_; } // Auto-reset; set when input is queued (message-thread mode).
    // Scripted input: the message handled now, on this thread, as the window procedure would handle it (a SendMessage of
    // the same message in the single-thread mode).
    LRESULT Deliver(UINT message, WPARAM w, LPARAM l) noexcept;
    // Test hook: the window thread's procedure blocks for `milliseconds` (posted: the caller does not wait).
    bool StallWindow(unsigned milliseconds) noexcept;
    struct InputStats { std::uint32_t handled = 0, lost = 0; std::int64_t latency_sum = 0, latency_max = 0; }; // Queued -> handled (100 ns).
    void ResetInputStatistics() noexcept { input_stats_ = {}; input_since_ = InputTime(); } // Counts input queued from now on.
    const InputStats& InputStatistics() const noexcept { input_stats_.lost = input_lost_.load(std::memory_order_relaxed); return input_stats_; }
    // Input isolation for automated runs (docs/kinetic-hud.md, "Test windows"). While on, physical input never reaches
    // the window: Messages() drops every keyboard, mouse, touch, pen and IME message taken from the queue (where the
    // desktop's input always arrives), and a click does not activate the window (WM_MOUSEACTIVATE eats it). Scripted
    // input still works: SendMessage from this thread calls the window procedure directly, and PostTestInput posts a
    // message that Messages() admits because it was tagged. UI thread.
    void SetInputIsolation(bool isolate) noexcept { isolate_input_.store(isolate, std::memory_order_relaxed); }
    bool InputIsolation() const noexcept { return isolate_input_; }
    bool PostTestInput(UINT message, WPARAM w, LPARAM l) noexcept; // False: 16 tagged posts already waiting, or PostMessage failed.
    unsigned IsolatedInputs() const noexcept { return isolated_inputs_.load(std::memory_order_relaxed); } // Queued input messages dropped.
    // Converts one displayable frame (or retires it without GPU access when it cannot be presented in
    // order) and fills the reverse lease for the decoder.
    RenderStatus Convert(const core::VideoFrameDescriptor&, const platform::windows::D3D12VideoDecoder&,
                         platform::windows::VideoSurfaceReturn&) noexcept;
    RenderStatus Present(const clock::PredictiveMasterClock&, std::int64_t now, const PresentState& state) noexcept;
    // Low-power pacing (docs/session.md, "Low-power pacing"): when the presentation thread must call Present again, as the
    // last call left it. `precise`: a picture's deadline (the caller wakes early and spins the last stretch); else the
    // controls' next refresh or advance, which a timer's lateness does not harm. `at` <= the call's `now`: at once (a picture
    // due and waiting on the GPU, a staged image to retry, a resize, a landing, a picture the clock gives no deadline yet).
    // `picture`: a picture has a deadline after `at` (the wait must not be one that can return late).
    struct Wake { std::int64_t at = 0; bool precise = true, picture = false; };
    Wake NextWake() const noexcept { return wake_; }
    // Paused and settled (2026-11-03): no picture due, the controls drawn as they are and nothing in them that ends with time,
    // every GPU submission complete (no snapshot, tensor, capture or re-conversion to see finish). The next advance is then
    // `wait` (100 ns) away instead of 8.3 ms: what wakes the thread before that is input, a picture, a host's request.
    // 0: never longer than an advance (test scripts that act during a pause are checked by passes).
    void SetSettledWait(std::int64_t wait) noexcept { settled_wait_ = wait; }
    bool Settled() const noexcept { return settled_; }
    // Why the frame at the front of the decoded queue cannot be converted now, for the same pacing. Presentation: every RGB
    // slot holds a picture still to show, or the order is full (a flip frees one). Decode, Conversion: a GPU fence, the
    // picture's decode or the conversion in flight (ArmConvertWake sets an event on it). Now: convertible, or a queue seek's
    // adoption still to decide.
    enum class ConvertBlock : std::uint8_t { Now, Presentation, Decode, Conversion, Steadying };
    ConvertBlock ConvertWait(const core::VideoFrameDescriptor&, const platform::windows::D3D12VideoDecoder&) const noexcept;
    bool ArmConvertWake(ConvertBlock, const core::VideoFrameDescriptor&, const platform::windows::D3D12VideoDecoder&, HANDLE event) noexcept;
    // Spatial stage of the conversion (ColorPipeline): edge-directed luma while upscaling (else bilinear) and
    // contrast-adaptive sharpening strength in [0, 1] (0 = off). Takes effect from the next converted frame.
    void SetPicture(bool edge_directed, float sharpness) noexcept {
        reconstruction_ = edge_directed ? kEdgeDirected : kBilinear; sharpness_ = sharpness < 0 ? 0.0f : sharpness > 1 ? 1.0f : sharpness;
    }
    // Picture controls and micro-motion (docs/picture.md, "Picture controls and micro-motion"; Reconstruct.hlsli): brightness
    // -0.5..0.5, contrast 0..2, saturation 0..2 (0: monochrome), gamma 0.5..2.5, shadow lift 0..1, and the micro-motion gain
    // (1 or less: off; at most 8). Clamped. From the next converted picture; a paused picture converts again (SettingsChanged).
    // Turning micro-motion on starts its per-pixel reference over; so do a new epoch, a new zoom region and a new picture size.
    struct PictureControls { float brightness = 0, contrast = 1, saturation = 1, gamma = 1, shadow_lift = 0, micro_motion = 1; };
    // Modules (docs/sdk.md, "Modules"). Before Prepare: without the HUD module the dock is neither drawn nor touched; without
    // the neural module no 2x luma texture or network is made (the Neural tier converts as Edge-directed).
    void EnableHud(bool on) noexcept { hud_enabled_ = on; }
    void AllowNeural(bool on) noexcept { neural_allowed_ = on; }
    // The text threads running for this media: the command bar's, the status cues' and the subtitle shaper.
    unsigned TextThreads() const noexcept { // And perception's (2026-11-01).
        return (palette_pipeline_.Ready() ? 1u : 0u) + (status_pipeline_.Ready() ? 1u : 0u) + (subtitle_track_ ? 1u : 0u) + (perceive_thread_ ? 1u : 0u);
    }
    void SetPictureControls(const PictureControls&) noexcept;
    const PictureControls& PictureControlsNow() const noexcept { return picture_; }
    // Rotation (docs/picture.md, "Rotation", 2026-10-28). The file's own orientation (its video track's tkhd matrix,
    // IsobmffParser's TrackInfo::transform: a, b, c, d; before Prepare, which shapes the window by it), then the viewer's
    // quarter turns (R, Alt+R; "rotate video") and leveling turn (Ctrl+R, Ctrl+Shift+R: a degree; "level horizon right"), at most
    // kMaxLevel either way, the view zoomed in just enough to keep the turned picture's corners inside the crop. All of it is
    // the conversion's own mapping (ColorConstants kRotate): no pass, no allocation; a paused picture is converted again.
    static constexpr float kMaxLevel = 15.0f;
    void SetOrientation(const std::int8_t transform[4]) noexcept;
    void Rotate(int quarter_turns, bool cue = true) noexcept;           // Clockwise (negative: counter-clockwise).
    void Level(float degrees, bool relative, bool cue = true) noexcept; // Clockwise; relative: added to the present turn.
    unsigned QuarterTurns() const noexcept { return orientation_.QuarterTurns(); } // Shown, clockwise (the file's and the viewer's).
    bool Mirrored() const noexcept { return orientation_.Mirrored(); }
    float LevelDegrees() const noexcept { return orientation_.level; }
    const video::Orientation& OrientationNow() const noexcept { return orientation_; }
    unsigned Rotations() const noexcept { return rotations_; }          // Changes made (tests).
    // Auto-leveling (docs/picture.md, "Auto-leveling the horizon", 2026-10-29; Shift+R, "auto level"): each conversion's
    // statistics pass also histograms its edges' orientations (ColorPipeline kHorizon); the 1024 bytes are read with the
    // statistics once the picture's fence has passed (no picture leaves the GPU) and video::HorizonLevel finds the stored
    // picture's tilt, the median of the last ones, which the leveling turn follows (a glide, within kMaxLevel; a mirrored view
    // the other way). A manual level (Ctrl+R, "level horizon right", "straighten") turns it off; a new file starts it over.
    void SetAutoLevel(bool on, bool cue = true) noexcept;
    bool AutoLevel() const noexcept { return auto_level_; }
    const video::HorizonLevel& Horizon() const noexcept { return horizon_; } // Its measurements (tests, the telemetry panel).
    unsigned HorizonReadbacks() const noexcept { return horizon_reads_; }   // Histograms read (1024 bytes each).
    // Black bars (docs/picture.md, "Black bars", 2026-10-30): every conversion's statistics pass also finds the picture's extents
    // (ColorPipeline: 16 bytes with the statistics); pictures converted whole (no zoom region: a zoomed view shows no bars) feed
    // video::Matte, the bars encoded in the picture (letterbox, pillarbox), in the stored crop's samples. Auto-crop ("crop black
    // bars", B) zooms the view so that the picture inside them fills it (the zoom's region inside the active area, the crop's
    // shape: a 2.39:1 film in a 16:9 window loses its sides), and follows the active area while on; a zoom by hand turns it off,
    // and off the view is whole again. A new file starts the bars over (auto-crop stays as the viewer left it).
    void SetAutoCrop(bool on, bool cue = true) noexcept;
    bool AutoCrop() const noexcept { return auto_crop_; }
    bool AutoCropped() const noexcept { return auto_crop_ && cropped_changes_ != 0 && matte_.Bars(); } // The view shows the active area now.
    const video::Matte& Bars() const noexcept { return matte_; }
    SampleRect ViewRegion() const noexcept { return RegionOf(history_crop_); } // The region of the crop the view shows now (zoom, crop).
    // Stabilising the window (docs/picture.md, "Stabilising the window", 2026-11-04; Alt+S, "stabilize video", the SDK): before each
    // picture is converted, its grid is made on the GPU and read back (a list of its own, kGridBytes in its slot's readback), the
    // virtual IMU on perception's thread measures the scene's motion since the picture before it (in decode order), and
    // video::Stabilizer turns that into the view: at least a 1.25x crop, its centre and turn following the shake, a pan passing
    // through. The conversion then reconstructs exactly that view from the picture's own samples through its affine map
    // (kRotate, as a leveling turn), so nothing is enlarged afterwards. Any thread; the presentation thread applies it, with a
    // cue, and the queued pictures go through the live zoom's blit to the new crop. False: no grid or no perception thread here (a
    // picture under 128 samples a side, or the telemetry module left out).
    bool SetStabilization(bool on) noexcept;
    [[nodiscard]] bool Stabilization() const noexcept { return steady_wanted_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool StabilizationPossible() const noexcept { return grid_data_ && perceive_thread_ && grid_imu_.Configured(); }
    [[nodiscard]] std::uint32_t SteadiedPictures() const noexcept { return steadied_; } // Converted on the steadied view.
    [[nodiscard]] std::uint32_t SteadyMeasured() const noexcept { return steady_measured_.load(std::memory_order_relaxed); } // Of them, with a valid motion.
    unsigned MicroPictures() const noexcept { return micro_pictures_; } // Conversions with micro-motion on (folded or not).
    // Each picture as it is put on screen, with what the conversion's statistics pass measured of it (docs/sdk.md, "Frame
    // signatures"): mean and brightest luma (0..1, of 8x8-block samples), the share of 8x8 blocks whose sample moved by more
    // than 1.2 % since the picture converted before it (-1: unknown, the first of an epoch or zoom region), and whether the
    // live cut detector confirmed a scene cut with it (the detector sees CutDetector::kDepth pictures past a cut before it
    // decides: `cut_pts` is the cut's own picture). `measured` false: its statistics were not read back in time (the luma
    // fields are -1). UI thread, called inside the presentation pass: the sink must be quick (the engine copies it).
    struct PictureSignature {
        std::int64_t pts_100ns = 0, duration_100ns = 0, presented = 0; // `presented`: InputTime() at the flip (100 ns).
        std::int64_t cut_pts = -1;
        std::uint32_t epoch = 0;
        float luma_mean = -1, luma_peak = -1, motion = -1;
        bool cut = false, measured = false;
    };
    using SignatureSink = void (*)(void* context, const PictureSignature&) noexcept;
    void SetSignatureSink(SignatureSink sink, void* context) noexcept { signature_sink_ = sink; signature_context_ = context; }
    // Reconstruction quality (docs/picture.md, "Quality governor"). Auto (the default): QualityGovernor chooses the tier
    // (Neural super-resolution, Edge-directed + guided chroma, Fast bilinear) from the GPU time of each conversion,
    // measured with timestamp queries on this queue, so that it fits the frame interval; each change it makes shows a
    // status cue. The other modes fix the tier. The N key cycles Auto -> Neural -> Edge-directed -> Fast -> Auto (with a
    // cue). Takes effect from the next converted frame (no stall, no allocation). Neural 2x luma (micro-ESPCN,
    // NeuralUpscalePass) runs only while upscaling, when Prepare knew the video size and the video can be shown larger
    // than its source (NeuralAvailable()); otherwise the Neural tier converts as Edge-directed.
    void SetQualityMode(QualityMode mode) noexcept;
    QualityMode Quality() const noexcept { return governor_.Mode(); }
    QualityTier Tier() const noexcept { return governor_.Tier(); } // For the next conversion (before the neural fallback).
    const QualityGovernor& Governor() const noexcept { return governor_; }
    bool Timing() const noexcept { return timestamps_ != nullptr; } // GPU timestamps available on this queue.
    std::int64_t LastGpuTime() const noexcept { return last_gpu_; } // The latest measured conversion (100 ns).
    unsigned TierFrames(QualityTier tier) const noexcept { return tier_frames_[static_cast<unsigned>(tier)]; } // Converted per tier, as run.
    // Test hook: extra GPU work in every SDR conversion at the Neural and Edge-directed tiers, about `milliseconds`
    // (the conversion dispatch repeated, calibrated from its own measured time): real load on the GPU, measured by the
    // same timestamps, so the governor's response to overload can be tested on any GPU. 0: off. UI thread.
    void SetTestLoad(float milliseconds) noexcept { test_load_ = milliseconds > 0 ? milliseconds : 0.0f; }
    bool NeuralAvailable() const noexcept { return neural_texture_ != nullptr; }
    // Live scene cuts (docs/chapters.md, "Live cuts"): every converted picture's mean luma (and, for HDR, the tone mapper's
    // own cut detection) read back once its conversion completes, compared in presentation order (timeline::CutDetector),
    // restarted at every epoch. UI thread: the cuts found so far (a ring of the newest kCutRing), read from `cursor` on.
    static constexpr unsigned kCutRing = 16;
    unsigned Cuts() const noexcept { return cut_count_; }
    bool NextCut(unsigned& cursor, std::int64_t& pts) const noexcept {
        if (cursor < cut_count_ && cut_count_ - cursor > kCutRing) cursor = cut_count_ - kCutRing; // Overrun: the oldest are gone.
        if (cursor >= cut_count_) return false;
        pts = cuts_[cursor++ % kCutRing]; return true;
    }
    float LastLuma() const noexcept { return last_luma_; } // Mean luma (0 to 1) of the last picture read back.
    unsigned LumaPictures() const noexcept { return luma_pictures_; }
    unsigned NeuralFrames() const noexcept { return neural_frames_; } // Frames converted through the neural pass.
    // Of those, the ones allowed to reuse unchanged tiles of the previous one (temporal tile reuse, docs/neural.md).
    unsigned NeuralReuseFrames() const noexcept { return neural_pass_.ReuseRecords(); }
    // Temporal tile reuse for playback: tolerant by default (docs/neural.md); Exact and Off for comparison.
    void SetNeuralReuse(NeuralUpscalePass::TemporalReuse mode) noexcept { neural_reuse_ = mode; neural_pass_.SetReuse(mode); }
    NeuralUpscalePass::TileCounts NeuralTiles() const noexcept { return neural_pass_.TileStatistics(); }
    // GPU time of the conversions made through the neural pass (pass and conversion, timestamps), 100 ns, and their number.
    struct GpuTotal { std::int64_t sum = 0; unsigned count = 0; };
    GpuTotal NeuralGpu() const noexcept { return neural_gpu_; }
    // Luma-guided chroma upsampling (Reconstruct.hlsli) in place of bilinear 4:2:0 chroma: colour edges follow the
    // reconstructed luma edges instead of bleeding. On by default; the C key toggles it; next converted frame.
    void SetGuidedChroma(bool enabled) noexcept { guided_chroma_ = enabled; SettingsChanged(); }
    // Split comparison (V, or "compare quality"): the left half of the picture converted plain (bilinear, no sharpening),
    // the right half as the quality tier converts it, a seam between them, from the next converted picture. `cue`: show
    // "Comparison · Original | Enhanced" (or "Comparison off").
    void SetComparison(bool on, bool cue = true) noexcept;
    bool Comparison() const noexcept { return compare_; }
    // Comparisons (V cycles them, docs/picture.md "Pinned | Live"): Original | Enhanced (as above), then, once a reference
    // frame is pinned, Pinned | Live: the left half of the picture is the pinned frame (t1), the right half the picture on
    // screen now (t2), with a seam between them. The pinned frame is composed as the live one is (copied when its size is
    // the picture's, else scaled), before the controls.
    enum class Compare : std::uint8_t { Off, Enhanced, Pinned };
    void SetCompare(Compare, bool cue = true) noexcept;
    Compare CompareMode() const noexcept { return compare_mode_; }
    void CycleCompare() noexcept; // V: Off -> Enhanced -> Pinned (with a reference) -> Off.
    // The picture on screen (the history: the last picture presented, or the paused one) as the reference frame. False:
    // nothing shown yet, or the previous copy is still on the GPU.
    bool PinReference() noexcept;
    bool ReferencePinned() const noexcept { return reference_valid_; }
    std::int64_t ReferencePts() const noexcept { return reference_pts_; }
    unsigned PinnedCompareComposites() const noexcept { return pinned_compare_composites_; }
    // ---- Pinned loop turnaround (docs/seeking.md, "Pinned loop turnaround") ----
    // While an A–B loop is set (ArmLoopPins), the pictures shown from the one holding A on, consecutive and presented, are
    // copied into pinned textures as they reach the screen: at most kPins, within kPinSpan of A, ending at least kPinMargin
    // before B. A later turn back to A (BeginPinnedEpoch) puts copies of them into the new epoch's presentation queue at
    // once, so A is on screen within a pass of the presentation loop; the decoder meanwhile decodes from the sync sample up
    // to LoopPinEnd(), behind them. No allocation: the textures exist from Prepare.
    static constexpr unsigned kPins = 8;
    static constexpr std::int64_t kPinSpan = 4500000, kPinMargin = 1000000;
    void ArmLoopPins(std::int64_t a, std::int64_t b) noexcept;
    void ClearLoopPins() noexcept;
    bool LoopPinsReady() const noexcept { return pin_complete_ && pin_count_ != 0; }
    unsigned LoopPins() const noexcept { return pin_complete_ ? pin_count_ : 0; }
    std::int64_t LoopPinEnd() const noexcept;   // The end of the last pinned picture (where decoding takes over).
    // Like BeginEpoch(epoch, now, target) for a seek to the pinned A, then the pinned pictures queued in it, copied into free
    // slots on a copy queue of their own (beside the direct queue's conversions, which would otherwise delay the landing).
    // `end`: where the pictures queued end (the decoder takes over there; fewer pins when fewer slots are free). False
    // (nothing changed): no pins, no free slot, or the previous copies still running.
    bool BeginPinnedEpoch(std::uint32_t epoch, std::int64_t now, std::int64_t target, std::int64_t& end) noexcept;
    unsigned PinnedTurns() const noexcept { return pinned_turns_; }
    // The landing staged ahead (2026-11-02; docs/seeking.md, "Pinned loop turnaround"): called as the clock nears B, it
    // copies A's first pin into a free slot held out of the queue, so the turn finds its landing's copy run (BeginPinnedEpoch
    // takes it; any other new timeline, or the loop changing, gives the slot back). True when one is staged.
    bool StageLoopLanding() noexcept;
    unsigned StagedLandings() const noexcept { return staged_landings_; }
    unsigned StagedTurns() const noexcept { return staged_turns_; } // Pinned turns that took a staged landing.
    // Test hooks (EnableCapture): the picture as composed on screen, before the controls (its video rectangle), and the
    // reference frame, hashed in halves as RequestCapture's are. Capture::kind says which.
    void RequestScreenCapture() noexcept { screen_capture_requested_ = capture_enabled_; }
    void RequestReferenceCapture() noexcept { reference_capture_requested_ = capture_enabled_; }
    unsigned ComparisonFrames() const noexcept { return compare_frames_; } // Pictures converted split (Original | Enhanced).
    bool GuidedChroma() const noexcept { return guided_chroma_; }
    // Seek (UI thread): frames of earlier epochs are dropped from the reorder window and never presented; the
    // first frame of `epoch` is presented as soon as presentation order allows, without waiting for the clock
    // or for resume, and later frames follow the re-anchored clock. `now` timestamps the request.
    // That landing frame shows the media at `target`: when the target lies inside the frame's interval, captions and
    // controls are those of the target rather than of the frame's start (a seek to a cue's start shows that cue).
    //
    // Queue seek (`adopt`, docs/seeking.md "Queue seek"): a forward target among pictures already decoded. Converted
    // pictures that end at or before the target are dropped; the later ones stay, now in `epoch`, and the first of them
    // (the one showing the target) lands at once. Pictures of the old decode chain (epochs from `chain` on) still to
    // arrive are adopted the same way: those that end at or before the target are stale, the others join `epoch`.
    // True when a converted picture after the target was kept: the adoption is then certain (the worker must keep the
    // decode chain). Otherwise it is pending until ResolveAdoption, and old-epoch pictures wait in Convert.
    bool BeginEpoch(std::uint32_t epoch, std::int64_t now, std::int64_t target = INT64_MIN, bool adopt = false, std::uint32_t chain = 0) noexcept;
    void ResolveAdoption(bool adopted) noexcept; // The worker's decision for the latest queue seek (kept or pending).
    unsigned AdoptedFrames() const noexcept { return adopted_frames_; } // Pictures of an earlier epoch shown in a later one.
    struct SeekPresentation { std::uint32_t epoch = 0; std::int64_t pts = 0, duration = 0, latency = 0, presented = 0; SubtitleFrame subtitles{}; std::uint8_t waited = 0; }; // waited: Wait mask.
    // Most recent epoch whose first frame reached the screen: its PTS and request-to-present latency (100 ns).
    const SeekPresentation& LastSeek() const noexcept { return seek_; }
    std::uint32_t Epoch() const noexcept { return epoch_; }
    // Newest epoch with a converted picture whose GPU work has completed (0: none yet). Playback start waits for it:
    // the clock must not run before the first picture can be shown (Primed -> Running).
    std::uint32_t ReadyEpoch() const noexcept { return ready_epoch_; }
    // Cold start (docs/seeking.md, "Pre-roll and the presentation queue"): Prepare puts a new swap chain's first image (black)
    // through the compositor and waits for it (DwmFlush), so that its first Present and first composition, the slow ones,
    // are over before playback and its clock start. A warm session's swap chain has composed images already: none.
    std::int64_t WarmUp() const noexcept { return warm_up_; }               // Prepare's warm-up flip, Present to composed (100 ns; 0: none).
    std::int64_t WarmUpPresent() const noexcept { return warm_up_present_; } // Of that, until its Present returned.
    HRESULT WarmUpResult() const noexcept { return warm_up_result_; }       // DwmFlush's.
    unsigned Stale() const noexcept { return stale_; } // Converted frames flushed by a seek, never presented.
    bool Empty() const noexcept { return order_.Size() == 0; }
    unsigned Presented() const noexcept { return presented_; }
    unsigned Dropped() const noexcept { return dropped_; }          // Presentation interval already passed.
    // Diagnostics for the most recent drop: the frame's PTS and how late it was (100 ns); `behind_clock`: its PTS was
    // already behind the clock when it reached the window (out of the clock's range), rather than a missed deadline.
    // What a due picture waited for before it could be handed to the swap chain (a mask, accumulated while it waited).
    enum Wait : std::uint8_t {
        kWaitConversion = 1,  // Its own conversion had not finished on the GPU.
        kWaitBackBuffer = 2,  // The back buffer's previous image was still being drawn.
        kWaitLatency = 4,     // The swap chain's frame-latency object: the flips already queued had not been shown.
        kWaitPresent = 8,     // Present itself was still drawing.
    };
    struct Drop { std::int64_t pts = 0, lateness = 0; bool behind_clock = false; std::uint8_t waited = 0; };
    struct LateFlip { std::int64_t pts = 0, lateness = 0; std::uint8_t waited = 0; };
    const Drop& LastDrop() const noexcept { return last_drop_; }
    // The latest any picture was handed to the swap chain after its clock deadline (previews have none).
    std::int64_t WorstLateness() const noexcept { return worst_flip_.lateness; }
    const LateFlip& WorstFlip() const noexcept { return worst_flip_; }
    // The start-up trace (2026-11-03; docs/fixtures.md, "Cold starts"): the session's first kTrace conversions (when the
    // picture reached ConvertFrame, when its decode was first seen complete, when its conversion was queued, its GPU time and
    // when that was read, its tier, the other slots busy then) and the first kTrace timed pictures' fates (the deadline, when
    // they were handed to the swap chain or dropped, what they waited for). InputTime, 100 ns. Fixed storage.
    static constexpr unsigned kTrace = 96;
    struct TraceConversion { std::int64_t pts = 0, seen = 0, decoded = 0, queued = 0, gpu = -1, harvested = 0; std::uint8_t tier = 0, busy = 0; };
    struct TracePicture { std::int64_t pts = 0, deadline = 0, at = 0; std::uint8_t waited = 0; bool dropped = false; };
    unsigned TraceConversions(const TraceConversion*& out) const noexcept { out = trace_conversions_; return trace_converted_; }
    unsigned TracePictures(const TracePicture*& out) const noexcept { out = trace_pictures_; return trace_pictured_; }
    unsigned OutOfOrder() const noexcept { return out_of_order_; }  // Late/duplicate PTS rejected by the window.
    // Diagnostics for the most recent rejection: the picture's PTS and epoch, the window's last released PTS then
    // (INT64_MIN: none), the reason (video::ReorderInsert) and the renderer's epoch.
    struct OrderReject { std::int64_t pts = 0, last_released = 0; std::uint32_t epoch = 0, renderer_epoch = 0; std::uint8_t reason = 0; bool pinned = false; };
    const OrderReject& LastOrderReject() const noexcept { return last_reject_; }
    std::uint64_t Converted() const noexcept { return converted_; }
    unsigned ReorderDepth() const noexcept { return order_.Depth(); }
    unsigned ToneMapped() const noexcept { return tone_mapped_; } // Frames converted by the PQ/HLG pass.
    HRESULT LastError() const noexcept { return error_; }
    bool Close() noexcept; // Quiescent teardown only; false quarantines native GPU objects.
    // Window handoff. Adopt (before Prepare): Prepare takes that window, at its client size, instead of creating one (a
    // window that no longer exists is replaced by a new one). KeepWindow (before Close): Close leaves the window open,
    // at its place and size, and HandedWindow then describes it for the next renderer's Adopt.
    void Adopt(const WindowHandoff& handoff) noexcept { adopt_ = handoff; }
    void KeepWindow() noexcept { keep_window_ = true; }
    const WindowHandoff& HandedWindow() const noexcept { return handoff_; }
    bool Adopted() const noexcept { return adopted_; }
    unsigned KeptSlots() const noexcept { return kept_slots_; } // 1: this file took a warm session's slots and history.
    // Device loss (TDR, driver update or restart, adapter removal, ID3D12Device5::RemoveDevice): Convert and Present
    // return DeviceLost, and a health probe in Present notices a removal within 250 ms even while nothing is drawn.
    // Recovery runs on this thread with every decode-side user of the device quiescent:
    // - ReleaseDevice drops every GPU object and the device itself without waiting on the removed device (it can
    //   retire nothing more); `remaining` is the reference count left on the device, 0 unless something leaked.
    // - RecreateDevice builds them again on a new device for the same window, sizes, controls, subtitles and picture
    //   settings, without arena use; then Bind the rebuilt decoder.
    // The picture on screen went with the device: the next frame of the current epoch is shown as soon as it is in
    // order, even while paused (like a seek's first frame), and recorded as RecoveryLanding.
    [[nodiscard]] bool Lost() const noexcept;
    [[nodiscard]] bool ReleaseDevice(ULONG& remaining) noexcept;
    [[nodiscard]] bool RecreateDevice() noexcept;
    unsigned Recoveries() const noexcept { return recoveries_; }
    const SeekPresentation& RecoveryLanding() const noexcept { return recovery_landing_; } // latency: from RecreateDevice.
    bool PreviewPending() const noexcept { return preview_; } // The current epoch's first frame has not been shown yet.
    std::int64_t ShownPts() const noexcept { return shown_pts_; } // Last video frame presented (INT64_MIN: none yet).
    std::int64_t ShownDuration() const noexcept { return shown_duration_; } // Its duration (100 ns; 0: none yet).
    // Region-of-interest zoom (docs/picture.md, "Zoom"): 1x to 8x into the picture, around a point given as a fraction of
    // the crop (0.5, 0.5: its centre). The region is cropped before reconstruction: the conversion, the neural pass and
    // guided chroma all run on the region's own samples, scaled to the whole picture area, so a 4x zoom is reconstructed
    // at 4x the scale rather than enlarged after it. The region keeps the crop's shape, on even sample positions (4:2:0),
    // inside the crop. Ctrl+wheel zooms at the pointer, the arrow keys pan while zoomed. Paused, the picture on screen is
    // converted again at once (as for the other picture settings). A status cue says the zoom.
    static constexpr float kMaxZoom = 8.0f;
    void SetZoom(float zoom, float x, float y, bool cue = true) noexcept;
    void ZoomBy(float factor, float x, float y, bool cue = true) noexcept; // Keeps the point at fraction (x, y) of the view under itself.
    void Pan(float dx, float dy) noexcept;   // Moves the region by fractions of its own size.
    float Zoom() const noexcept { return zoom_; }
    float ZoomX() const noexcept { return zoom_x_; }
    float ZoomY() const noexcept { return zoom_y_; }
    // Target tracking (docs/picture.md, "Target tracking"): on, the next converted picture's patch at the centre of the view
    // is the target (the zoom goes to 2x when it was off); each conversion records the crop's luma grid (HdrToneMap.hlsl Grid),
    // and when it has run the tracker moves the view's centre for the conversions after it. T at 1x zooms to 2x at once (a
    // live zoom: the next composite shows it). While tracking, a picture converted following the target is shown with the
    // region it was converted from (its own framing); one converted before the lock (the queue at T) is shown through the
    // tracking view at once, sampled out of what it holds (live zoom), centred where the tracker's estimate and velocity put
    // the target at that picture's time. Lost, the view holds.
    void SetTracking(bool on) noexcept;
    [[nodiscard]] bool Tracking() const noexcept { return tracker_.Active() || track_pending_; }
    [[nodiscard]] const video::TrackStatus& TrackState() const noexcept { return tracker_.Status(); }
    [[nodiscard]] std::uint32_t TrackEvents() const noexcept { return track_events_; } // Locks, losses and finds so far.
    [[nodiscard]] std::int64_t TrackedPts() const noexcept { return tracked_pts_; } // The picture the tracker last measured.
    [[nodiscard]] bool TrackLockFailed() const noexcept { return track_lock_failed_; } // The last lock found no texture.
    // Several objects at once (docs/picture.md, "Several objects at once", 2026-10-31): while tracking, video::Perception runs on
    // each grid the tracker reads (the crop's luma at the tracker's cell size, in conversion order), and while not, on a grid
    // every kWarmInterval (2026-11-01: the table is ready when T is pressed, and T's target is the object at the lock point at
    // once), so NextTarget ("next target"; Tab while tracking, the very next picture after T too) moves the lock and the view
    // (the audio focus with it) onto the next object of its table,
    // the most urgent first (the first when the target followed is none of them). The view moves at once; the pictures already
    // converted keep their framing; the lock is taken on the next converted picture, at the object. PrepareTargets (Preparing)
    // takes Perception's buffers from `arena` (Perception::ArenaBytes of the grid's width: at most 480 cells, about 325 KiB);
    // without it, or for a grid under Perception::kMinSize cells a side, there are no objects and NextTarget does nothing.
    bool PrepareTargets(core::LinearMemoryArena& arena) noexcept;
    [[nodiscard]] bool TargetsReady() const noexcept { return targets_ready_; }
    bool NextTarget() noexcept; // False: no object to move to (none, or the one followed is the only one).
    // The newest table perception's thread published (2026-11-01: perception runs on its own thread, PerceiveThread).
    [[nodiscard]] const video::PerceptionResult& Objects() const noexcept { return perceived_[published_.load(std::memory_order_acquire)]; }
    [[nodiscard]] std::uint32_t TargetId() const noexcept { return target_id_; }    // The object the lock is on (0: none).
    // Remote vibrometry (docs/picture.md, "Remote vibrometry", 2026-11-02): while tracking (T) or with micro-motion on; the
    // newest result, the HUD readouts shown and the last one's text. Since 2026-11-03 ("Vibrometry in the window") each
    // converted picture's region is read on the GPU at full resolution, Y', Cb and Cr (ColorPipeline::RecordVibro), not on the
    // luma grid: the target's path while tracking (the region follows it, the window goes on), else the view's fovea.
    [[nodiscard]] const video::VibrometryResult& VibrometryNow() const noexcept { return vibro_.Result(); }
    [[nodiscard]] std::uint32_t VibrometryReadouts() const noexcept { return readouts_; }
    [[nodiscard]] const char* VibrometryReadout() const noexcept { return readout_; }
    // The event camera in the window (2026-11-03; docs/picture.md, "Event camera"): PrepareSensing (Preparing, the telemetry
    // module's region) makes its sensor and a virtual IMU at the tracking grid's size (at most 480 x 272 cells: 931 KiB since
    // 2026-11-04, the sensor reading the grid in place, its ring of kEventRing events).
    // SetEventCamera (any thread) turns it on with options, off with null; while on, every converted picture's grid is
    // recorded and handed to perception's thread (2026-11-03: on the presentation thread a Debug build dropped 48 of 70
    // pictures), which gives it to the sensor in conversion order, stabilised (unless asked not to be) by the virtual IMU's
    // motion between consecutive grids (video::VirtualImu: the background's, the perceived objects left out; while the view is
    // stabilised, the motion measured before each conversion, added up over skipped grids). A grid that
    // comes while the thread is still busy is skipped (counted; the next interval is then longer). Events(): its ring, one
    // consumer thread; EventState(): its counters as published after each picture.
    bool PrepareSensing(core::LinearMemoryArena& arena) noexcept;
    bool SetEventCamera(const video::EventCameraOptions* options) noexcept; // False: no sensor here.
    [[nodiscard]] video::EventCamera& Events() noexcept { return events_; }
    struct EventCameraState { bool configured = false, on = false; unsigned width = 0, height = 0, scale = 1; std::uint32_t pictures = 0, rebases = 0, last = 0, skipped = 0; std::uint64_t events = 0, dropped = 0; };
    [[nodiscard]] EventCameraState EventState() const noexcept;
    [[nodiscard]] const video::VirtualImu& GridImu() const noexcept { return grid_imu_; }
    // Tensor export in the window (2026-11-03; docs/picture.md, "Tensor export"): a host's thread asks for a region of the next
    // picture converted (RequestTensor: the cells' buffer and readback made or grown on that thread, at most kMaxTensorCells
    // cells), the presentation thread records the pass (ColorPipeline::RecordTensor) with that conversion, and the asking
    // thread waits for its fence (WaitTensor), then converts the cells' Y'CbCr means (video::ExportTensorMeans) and lets the
    // request go (FinishTensor). One request at a time.
    // While paused (2026-11-04; docs/picture.md, "Tensors while paused") the region comes from the picture on screen: from the held
    // source when it is that picture (a pass of its own on the presentation thread, nothing decoded), else from the refresh landing
    // it asks for (one decode from the sync sample; the source is held from then on). A host that has exported a tensor before has
    // that landing made when the pause begins, so its first export while paused decodes nothing either.
    // Regions: kTensorFovea the target (its object's box grown by a fifth, else the tracker's fovea-sized window) while
    // tracking, else half the view across and down at its centre, shaped as the tensor; kTensorView the region converted;
    // kTensorFull the crop; kTensorCustom fractions of the crop.
    static constexpr std::uint32_t kMaxTensorCells = 1024u * 1024u;
    enum TensorKind : std::uint32_t { kTensorFovea = 0, kTensorView = 1, kTensorFull = 2, kTensorCustom = 3 };
    struct TensorAsk { std::uint32_t width = 0, height = 0, kind = kTensorFovea; float left = 0, top = 0, right = 1, bottom = 1; };
    struct TensorAnswer { video::AreaRect region{}; std::int64_t pts = -1; std::uint32_t target_id = 0; const float* means = nullptr;
                          video::TensorColor color{}; bool held = false; }; // `color`: what the means' codes are; `held`: from the held source.
    bool RequestTensor(const TensorAsk& ask) noexcept;           // False: busy, too large, or no device.
    int WaitTensor(TensorAnswer& answer, std::uint32_t timeout_ms) noexcept; // 1 ready, 0 not converted in time, -1 failed.
    void FinishTensor() noexcept;
    [[nodiscard]] std::uint32_t Retargets() const noexcept { return retargets_; }   // NextTarget moves so far.
    [[nodiscard]] std::uint32_t WarmGrids() const noexcept { return warm_grids_; }  // Grids perceived while not tracking.
    // Forensic snapshots (docs/picture.md, "Forensic snapshots", 2026-11-01): the picture as composed on screen (the window's
    // video rectangle: zoom, tracking, leveling, the turns, the split comparison, Pinned | Live; before captions, the dock, the
    // status cues and the command bar) copied by the next composite into a readback, for the engine's snapshot thread.
    // The readback (2026-11-02) is made when a snapshot is asked for, of the window's size (the video rectangle lies inside the
    // back buffer), by the snapshot thread (MakeSnapshotReadback: the device is free-threaded, so the presentation thread never
    // waits for it), and released when the files are written (DropSnapshotReadback): playing holds none (it held the slots'
    // capacity before: 33 MB for a 4K file). A window grown past it between the request and the composite hands the request
    // back (not held: the coordinator makes a larger one). RequestSnapshot: false while one is held, without a readback or
    // without a picture (music mode); it asks for a composite (paused, the picture held is composed again). SnapshotReady:
    // once the copy has completed, its pixels (valid until ReleaseSnapshot) and how they were shown.
    struct SnapshotImage {
        const std::uint8_t* rgba = nullptr; std::uint32_t stride = 0, width = 0, height = 0; // R G B A, rows of `stride` bytes.
        std::int64_t pts_100ns = -1;  // The picture's.
        SampleRect region{};          // The picture's region shown (samples of the coded picture).
        float zoom = 1, x = 0.5f, y = 0.5f, level = 0; std::uint32_t quarter_turns = 0; bool mirrored = false, auto_level = false;
        std::uint8_t compare = 0;     // 0 none, 1 Original | Enhanced, 2 Pinned | Live.
    };
    bool RequestSnapshot() noexcept;
    bool SnapshotReady(SnapshotImage& out) noexcept;
    [[nodiscard]] bool MakeSnapshotReadback(unsigned width, unsigned height) noexcept; // The snapshot thread's.
    void DropSnapshotReadback() noexcept;                                             // The snapshot thread's (and teardown's).
    [[nodiscard]] bool SnapshotPossible() const noexcept { return !music_ && history_valid_; }
    // The readback a snapshot of the window as it is needs (the back buffer's size, or the size it is being resized to).
    void SnapshotSize(unsigned& width, unsigned& height) const noexcept { width = (std::max)(width_, pending_width_); height = (std::max)(height_, pending_height_); }
    [[nodiscard]] bool SnapshotFits(unsigned width, unsigned height) const noexcept { return snapshot_data_ && width <= snapshot_width_ && height <= snapshot_height_; }
    [[nodiscard]] std::uint64_t SnapshotReadbackBytes() const noexcept { return snapshot_bytes_.load(std::memory_order_relaxed); } // Held now.
    [[nodiscard]] std::uint64_t SnapshotReadbackPeak() const noexcept { return snapshot_peak_.load(std::memory_order_relaxed); }  // The largest made.
    void ReleaseSnapshot() noexcept { snapshot_state_ = SnapshotState::Idle; }
    [[nodiscard]] bool SnapshotHeld() const noexcept { return snapshot_state_ != SnapshotState::Idle; }
    // The centre of the region of the picture on screen (fractions of the crop): what the audio focus follows.
    [[nodiscard]] float ShownX() const noexcept { return shown_x_; }
    [[nodiscard]] float ShownZoom() const noexcept { return shown_zoom_; } // The crop's width over the shown region's.
    [[nodiscard]] float ShownY() const noexcept { return shown_y_; }
    using Region = SampleRect;
    Region RegionOf(const core::VideoFrameDescriptor&) const noexcept; // The crop, or its zoomed region.
    Region RegionOf(const Region& crop) const noexcept;
    Region RegionAt(const Region& crop, float x, float y) const noexcept; // At the current zoom, centred at (x, y) of the crop.
    const Region& LastRegion() const noexcept { return last_region_; }  // Of the latest conversion.
    // Live zoom (docs/picture.md, "Live zoom"). Pictures already converted when the zoom or pan changes (up to Count
    // waiting in the presentation queue, and the held picture) are shown through the change at presentation: PictureBlit
    // samples the region asked for now out of the region each was converted from. The change is on screen from the next
    // composite (a controls-only one is drawn for it when no picture is due within a refresh), and pictures converted
    // after it are reconstructed from the new region's own samples. Each change made while playing is recorded:
    // `shown`: from the change to the Present of the first image showing it; `native`: to the Present of the first
    // picture converted at it; `bridged`: images shown through the transform in between (100 ns; 0: not yet).
    struct LiveZoom { std::uint32_t generation = 0; std::int64_t requested = 0, shown = 0, native = 0; unsigned bridged = 0; Region region{}; };
    static constexpr unsigned kMaxLiveZooms = 8;
    unsigned LiveZooms() const noexcept { return live_zoom_count_; }
    const LiveZoom& LiveZoomAt(unsigned i) const noexcept { return live_zooms_[i < kMaxLiveZooms ? i : kMaxLiveZooms - 1]; }
    unsigned TransformedComposites() const noexcept { return transformed_composites_; } // Images shown through a zoom transform.
    // Paused inspection (docs/picture.md, "Paused re-conversion"): a picture setting changed while paused (V, N, C, or the
    // commands and setters behind them) shows on the picture on screen at once. When the renderer holds that picture's
    // source (a landing converted while paused keeps a copy), it converts it again into the held image and refreshes the
    // screen; otherwise it asks for a refresh landing (TakeRefresh: the engine seeks to ShownPts(), the picture is
    // decoded again and converted with the new settings, and is held from then on).
    void SettingsChanged() noexcept;
    bool TakeRefresh() noexcept { const bool wanted = refresh_wanted_; refresh_wanted_ = false; refresh_in_flight_ = refresh_in_flight_ || wanted; return wanted; }
    // latency: from the change to the image's Present call; completed: to the GPU's completion of that image (100 ns).
    struct PausedUpdate { QualityTier tier = QualityTier::EdgeDirected; std::uint32_t chroma_mode = 0; bool compare = false, refresh = false; std::int64_t latency = 0, completed = 0; };
    static constexpr unsigned kMaxPausedUpdates = 16;
    unsigned PausedUpdates() const noexcept { return paused_update_count_; }
    const PausedUpdate& PausedUpdateAt(unsigned i) const noexcept { return paused_updates_[i < kMaxPausedUpdates ? i : kMaxPausedUpdates - 1]; }
    // Test hook (EnableCapture before Prepare): the image on screen while paused, as hashes of its two halves (left of the
    // centre's two columns, right of them); Captures() counts the ones done.
    void EnableCapture() noexcept { capture_enabled_ = true; }
    void RequestCapture() noexcept { capture_requested_ = capture_enabled_; }
    enum class CaptureKind : std::uint8_t { Held, Screen, Reference };
    // Test hook (EnableCapture and EnableSpotCapture before Prepare; docs/picture.md, "Stabilising the window"): each screen capture
    // finds the brightest thing shown, a spot's centroid in pixels of the video rectangle, instead of hashing the image.
    struct Spot { std::int64_t pts = 0; float x = 0, y = 0; unsigned pixels = 0; };
    static constexpr unsigned kMaxSpots = 256;
    void EnableSpotCapture() noexcept { capture_spots_ = true; }
    bool ScreenCaptureWaiting() const noexcept { return screen_capture_requested_ || capture_pending_; }
    unsigned SpotCount() const noexcept { return spot_count_; }
    const Spot& SpotAt(unsigned i) const noexcept { return spots_[i < kMaxSpots ? i : kMaxSpots - 1]; }
    // grid: the mean luma (BT.709, 0..255) of each of 4 x 4 equal cells, row-major (which way a picture is turned). With
    // EnableCaptureHorizon (auto-leveling's test), the captured image's own lines measured too (video::HorizonLevel on its
    // red channel: the tests' grey pictures), the shown picture's tilt.
    struct Capture { std::uint64_t left = 0, right = 0; unsigned width = 0, height = 0; CaptureKind kind = CaptureKind::Held; float grid[16]{};
                     video::HorizonLevel::Measurement horizon{};
                     video::Matte::Rect extents{}; bool extents_found = false; }; // EnableCaptureHorizon: the shown image's picture (black bars test).
    void EnableCaptureHorizon() noexcept { capture_horizon_ = true; }
    static constexpr unsigned kMaxCaptures = 8;
    unsigned Captures() const noexcept { return capture_count_; }
    const Capture& CaptureAt(unsigned i) const noexcept { return captures_[i < kMaxCaptures ? i : kMaxCaptures - 1]; }
private:
    // The conversion of `frame` from a `source_width` x `source_height` surface, fitted to the window, with the quality tier
    // and picture settings of now (shared by Convert and the paused re-conversion). `fold_peak`: an HDR picture of the
    // timeline (its presentation time steps the dynamic peak); a re-conversion adds no time.
    struct Conversion { ColorConstants constants{}; QualityTier tier = QualityTier::EdgeDirected; bool neural = false; unsigned width = 0, height = 0; SampleRect region{}; };
    // The view a picture is converted on while stabilising: its centre (fractions of the crop) and turn (radians, about it).
    struct SteadyView { float x = 0.5f, y = 0.5f, turn = 0; bool on = false; };
    Conversion Setup(const core::VideoFrameDescriptor& frame, unsigned source_width, unsigned source_height, std::int64_t now, bool fold_peak, const SteadyView& steady) noexcept;
    RenderStatus Reconvert() noexcept;       // The held source into the held image with the settings of now.
    RenderStatus CaptureShown() noexcept;    // RequestCapture's copy and, once it has run, its hashes.
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM) noexcept;
    // The window procedure's handling, on the thread that handles input (the window's, or the presentation thread's with a
    // window thread); `now`: when the message arrived.
    LRESULT Process(HWND, UINT, WPARAM, LPARAM, std::int64_t now) noexcept;
    LRESULT Forward(HWND, UINT, WPARAM, LPARAM) noexcept; // The window thread's procedure.
    // An input event to the ring; `texts`: the input method's result and composition strings (WM_IME_COMPOSITION).
    void Push(UINT, WPARAM, LPARAM, const wchar_t* result = nullptr, unsigned result_length = 0, const wchar_t* composition = nullptr,
              unsigned composition_length = 0) noexcept;
    int Admit(const MSG&) noexcept; // Input isolation: 0 dropped, 1 a tagged test post (not translated), 2 as usual.
    bool Down(int key) const noexcept; // A modifier key as it was when the message being handled arrived.
public:
    // The same for a key handler (ConfigureKeys) while the message is handled: the demo's Shift+S.
    [[nodiscard]] bool ModifierDown(int key) const noexcept { return Down(key); }
private:
    void Ime(unsigned mode) noexcept;  // 0 off, 1 on, 2 off and the composition cancelled: on the window's thread.
    static DWORD WINAPI WindowThread(void*) noexcept;
    // Embedding: the parent's moves and resizes (EVENT_OBJECT_LOCATIONCHANGE, out of context, on the child's thread) resize the child.
    static void CALLBACK ParentChanged(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time) noexcept;
    static HWND CreateChild(HWND parent, int width, int height, void* renderer, HWINEVENTHOOK& hook) noexcept;
    static LRESULT HitTest(HWND, LPARAM, bool fullscreen, float dpi) noexcept; // WM_NCHITTEST: resize edges, the drag strip.
    static DWORD WindowThreadId() noexcept;
    struct Slot {
        ID3D12Resource* rgb = nullptr;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* commands = nullptr;
        core::VideoFrameDescriptor frame{};
        std::uint64_t fence = 0;
        unsigned width = 0, height = 0; // Converted (letterboxed) picture size inside the slot.
        bool busy = false, staged = false;
        // GPU timing of the slot's last conversion (timestamps 2i, 2i + 1): read once its fence has completed.
        bool timed = false; QualityTier tier = QualityTier::EdgeDirected; unsigned repeats = 0;
        std::int16_t trace = -1; // Its conversion's entry in the start-up trace (-1: none).
        bool vibro = false, vibro_follow = false; // Its vibrometry samples wait in the readback (kVibroBytes times i), of this region (crop samples).
        video::AreaRect vibro_region{};
        bool stats = false; // Its scene statistics wait in the readback buffer (ColorPipeline::kStatsBytes at that times i).
        bool horizon = false; // Its edge-orientation histograms too (kHorizonBytes at kStatsBytes Count + that times i).
        bool grid = false;  // Its tracking grid waits in the grid readback (kGridBytes times i): its scale and size follow.
        // Measured before its conversion (Measure): the scene's motion since the picture converted before it (grid cells), for the
        // event camera, which would otherwise measure it again.
        bool steadied = false; float motion_x = 0, motion_y = 0, motion_turn = 0;
        std::uint32_t grid_scale = 1, grid_width = 0, grid_height = 0, grid_words = 0;
        std::uint32_t stats_epoch = 0;
        float luma = -1, peak = -1, motion = -1; bool cut = false, measured = false; // Read from them (PictureSignature).
        std::int64_t cut_pts = -1;
        Region crop{}, region{}; std::uint32_t zoom_generation = 0; // The region it was converted from, and the zoom then.
        Region view{};        // The region it was last composited with (its region, or the live view sampled out of it).
        bool tracked = false; // Converted while the tracker followed a target: its region is the tracker's framing.
        std::uint64_t copy_fence = 0; // Filled by the copy queue (a pinned loop picture): readable once it is reached.
        std::uint64_t workload = 0; std::int64_t budget = 0;
    };
    // Conversions completed by `completed` (this queue's fence) to the governor, and a cue when it adapts.
    void HarvestTimings(std::int64_t now, std::uint64_t completed) noexcept;
    void HarvestScenes(std::uint64_t completed) noexcept; // In conversion order (fence order), not slot order.
    void QualityCue(bool manual) noexcept;
    RenderStatus ConvertFrame(const core::VideoFrameDescriptor&, const platform::windows::D3D12VideoDecoder&,
                              platform::windows::VideoSurfaceReturn&) noexcept;
    // A picture's motion before its conversion (one stage a call; WouldBlock until `view` is ready; Ready with view.on false: none).
    RenderStatus Measure(const core::VideoFrameDescriptor&, unsigned slot, const platform::windows::D3D12VideoDecoder&, std::uint64_t completed, SteadyView& view) noexcept;
    void Steady() noexcept;             // Perception's thread: the measurement.
    void ApplyStabilization() noexcept; // The presentation thread: on or off, as asked.
    float ViewZoom() const noexcept { return steady_on_ && zoom_ < video::Stabilizer::kZoom ? video::Stabilizer::kZoom : zoom_; }
    RenderStatus PresentFrame(const clock::PredictiveMasterClock&, std::int64_t now, const PresentState&) noexcept;
    bool CreateGpu() noexcept;
    void ReleaseGpu() noexcept;
    bool WaitIdle() noexcept;
    RenderStatus Retire(Slot&, bool presented) noexcept;
    RenderStatus Composite(unsigned slot, std::int64_t now) noexcept;
    RenderStatus FinishPresent(std::int64_t now) noexcept;
    RenderStatus ApplyResize() noexcept;
    void UpdateRefresh() noexcept;
    void RequestResize(unsigned width, unsigned height) noexcept;
    void Fit(unsigned source_width, unsigned source_height, unsigned& width, unsigned& height) const noexcept;
    void HudAction(ui::HudAction) noexcept;
    bool PaletteMessage(UINT message, WPARAM, LPARAM, std::int64_t now, LRESULT& result) noexcept; // True: handled.
    void SubmitPalette() noexcept;
    void PastePalette() noexcept;
    void ImeComposition(LPARAM flags) noexcept;
    void PlaceIme() noexcept; // Candidate window under the caret.
    std::uint64_t SelectSubtitles(std::int64_t time, SubtitleFrame&) const noexcept; // Returns a change-detection stamp.
    std::int64_t MediaTime(const core::VideoFrameDescriptor&) const noexcept; // The frame's PTS, or the seek target it lands.
    bool LandingReady() const noexcept; // The window's front is the current seek's landing picture: it may leave at once.
    static std::uint64_t Stamp(const SubtitleFrame&) noexcept;
    std::int64_t InputTime() const noexcept;
    bool CreateShared() noexcept; // The session's objects (GpuSession), when it has none yet.
    void ReleaseFile() noexcept;  // Everything else.
    void ReleaseShared() noexcept;
    GpuSession own_;              // A renderer outside any session keeps its device-level objects here.
    GpuSession& gpu_;
    GpuSession* session_ = nullptr;
    HWND window_ = nullptr;
    ID3D12Device*& device_ = gpu_.device;
    ID3D12CommandQueue*& queue_ = gpu_.queue;
    ID3D12Fence*& fence_ = gpu_.fence;
    IDXGISwapChain3*& swap_ = gpu_.swap;
    ColorPipeline& color_ = gpu_.color;
    HudPipeline& hud_pipeline_ = gpu_.hud;
    VisualPipeline& visual_pipeline_ = gpu_.visual;
    VisualUniforms MusicUniforms(std::int64_t now) noexcept; // The smoothed sound for this composite.
    bool music_ = false, staged_visual_ = false, music_paused_ = false;
    audio::AudioPicture audio_picture_{};
    unsigned audio_pictures_ = 0, music_presents_ = 0;
    float shown_bands_[audio::AudioPicture::kBands]{}; float shown_level_ = 0, shown_peak_ = 0;
    std::int64_t music_advanced_ = 0;
    float volume_ = 1.0f; bool muted_ = false; // The engine's output gain (SetVolume).
    SdfSubtitlePipeline subtitles_;
    NeuralUpscalePass neural_pass_;
    ID3D12Resource* neural_texture_ = nullptr; // 2x luma of the crop, R16F; created in Prepare only if upscaling is possible.
    unsigned neural_width_ = 0, neural_height_ = 0, neural_frames_ = 0;
    bool guided_chroma_ = true;
    bool compare_ = false; // Split comparison (filter flag kCompare).
    NeuralUpscalePass::TemporalReuse neural_reuse_ = NeuralUpscalePass::TemporalReuse::Tolerant;
    GpuTotal neural_gpu_{};
    unsigned compare_frames_ = 0;
    QualityGovernor governor_;
    ID3D12QueryHeap* timestamps_ = nullptr;      // 2 per slot: before and after its conversion.
    ID3D12Resource* timestamp_readback_ = nullptr;
    const std::uint64_t* timestamp_data_ = nullptr; // Persistently mapped readback.
    std::uint64_t timestamp_frequency_ = 0;
    std::int64_t last_gpu_ = 0;
    ID3D12Resource* stats_readback_ = nullptr;
    const std::uint8_t* stats_data_ = nullptr; // Persistently mapped: per slot, the peak state from [8] to [40).
    std::uint32_t stats_cuts_ = 0, stats_sum_ = 0, stats_count_ = 0; // The totals at the last picture read back.
    timeline::CutDetector cut_detector_;
    // Target tracking: the grid UAV and its per-slot readback (persistently mapped), the tracker, the view it moves.
    static constexpr std::uint32_t kGridBytes = 120u * 4u * 272u; // 480 x 272 cells at most (video::GridScale).
    ID3D12Resource* grid_ = nullptr; ID3D12Resource* grid_readback_ = nullptr; const std::uint8_t* grid_data_ = nullptr;
    video::TargetTracker tracker_;
    bool track_pending_ = false, track_lock_failed_ = false;
    // Several objects (2026-10-31): perception on the tracker's grids, the object the lock is on, a lock asked for elsewhere.
    video::Perception targets_;
    bool targets_ready_ = false, retarget_ = false;
    float retarget_x_ = 0.5f, retarget_y_ = 0.5f;
    std::uint32_t target_id_ = 0, retargets_ = 0, targets_epoch_ = UINT32_MAX;
    void FollowObject() noexcept; // target_id_: the object holding the tracker's position.
    std::uint32_t ObjectAt(float x, float y) const noexcept; // The object holding (x, y) (its box and a twentieth around it); 0: none.
    // Warm perception (2026-11-01): while not tracking, a grid every kWarmInterval of media (the newest converted picture that
    // far from the last one; only with room on the GPU: the last conversion at most half the interval, not the Fast tier), so
    // that the table is there when T is pressed; perception takes one grid that often while tracking
    // too (perceived_pts_: the last one it took).
    static constexpr std::int64_t kWarmInterval = 1000000; // 100 ms: past Perception::kBaseline, within kPersistence of the next.
    std::int64_t warm_pts_ = INT64_MIN, perceived_pts_ = INT64_MIN; std::uint32_t warm_epoch_ = UINT32_MAX;
    std::uint32_t warm_grids_ = 0; // Grids perceived while not tracking.
    // Perception's thread (2026-11-01; started by PrepareTargets, joined by StopPerception): run on the presentation thread,
    // a push cost 0.4 ms in Release but several in Debug, ten times a second in every playback (pictures dropped). The
    // presentation thread copies the grid into perceive_cells_ (the telemetry region) and hands it over when the thread is idle
    // (perceive_state_ 0 -> 1); the thread owns targets_, pushes the grid and publishes the table into the other of perceived_
    // (published_), then goes idle (1 -> 0). A grid due while it is busy waits for the next one.
    static DWORD WINAPI PerceiveEntry(void*) noexcept;
    void PerceiveThread() noexcept;
    void StopPerception() noexcept;
    HANDLE perceive_thread_ = nullptr, perceive_wake_ = nullptr;
    std::atomic<int> perceive_state_{0}; std::atomic<bool> perceive_stop_{false};
    std::uint8_t* perceive_cells_ = nullptr; unsigned perceive_width_ = 0, perceive_height_ = 0;
    std::int64_t perceive_pts_ = 0; bool perceive_reset_ = false, reset_pending_ = false;
    video::PerceptionResult perceived_[2]{}; std::atomic<unsigned> published_{0};
    // Forensic snapshots (2026-11-01): the readback (made when asked: the window's size), the state, the copy's fence and what it holds.
    enum class SnapshotState : std::uint8_t { Idle, Requested, Copying, Ready };
    ID3D12Resource* snapshot_readback_ = nullptr; const std::uint8_t* snapshot_data_ = nullptr;
    unsigned snapshot_width_ = 0, snapshot_height_ = 0;
    std::atomic<std::uint64_t> snapshot_bytes_{0}, snapshot_peak_{0};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT snapshot_layout_{};
    SnapshotState snapshot_state_ = SnapshotState::Idle; std::uint64_t snapshot_fence_ = 0;
    SnapshotImage snapshot_{};
    bool track_zoomed_ = false; // T zoomed in (from 1x) at once: a lock that finds nothing puts the zoom back.
    std::uint32_t track_events_ = 0;
    std::int64_t tracked_pts_ = INT64_MIN;
    std::int64_t estimate_pts_ = 0, estimate_duration_ = 0; // The picture the tracker's estimate is for (its lock, then each update).
    Region TrackedView(const Slot&) const noexcept;        // A picture converted before the lock, through the tracking view.
    float grid_mean_ = -1, shown_x_ = 0.5f, shown_y_ = 0.5f, shown_zoom_ = 1.0f;
    void HarvestGrid(std::uint64_t completed) noexcept; // The tracker on the completed conversion's grid (one at a time).
    // Remote vibrometry (2026-11-02; on the GPU's samples since 2026-11-03): while tracking or with micro-motion on, a slot's
    // sample once its conversion has run; its HUD readout every kReadoutInterval of media.
    void Vibrate(const Slot& s) noexcept;
    static constexpr std::int64_t kReadoutInterval = 20000000;
    video::Vibrometry vibro_;
    bool vibro_on_ = false, vibro_tracking_ = false; std::uint32_t vibro_epoch_ = 0;
    // The GPU's samples (2026-11-03): a UAV buffer and a readback region per slot (persistently mapped), the sample assembled
    // from one, the tracked target's path (crop samples) and the locks that start the window over.
    static constexpr std::uint32_t kVibroBytes = video::kVibrometryCells * video::kVibrometryCells * ColorPipeline::kVibroValues * 4;
    ID3D12Resource* vibro_buffer_ = nullptr; ID3D12Resource* vibro_readback_ = nullptr; const std::uint8_t* vibro_data_ = nullptr;
    video::VibrometrySample vibro_sample_{};
    video::TargetPath vibro_path_;
    std::uint32_t locks_ = 0, vibro_lock_ = 0;
    // Stabilising the window: the list of each picture's grid, the stage of the picture being measured (0 none, 1 its grid on the
    // GPU, 2 on perception's thread, 3 measured), its slot (held for it), and the job handed to the thread (steady_job_: 0 idle, 1
    // asked, 2 done). The thread owns stabilizer_ and, while the view is stabilised, grid_imu_.
    video::Stabilizer stabilizer_;
    ID3D12CommandAllocator* steady_allocator_ = nullptr; ID3D12GraphicsCommandList* steady_commands_ = nullptr;
    std::atomic<bool> steady_wanted_{false}; bool steady_on_ = false, steady_restart_ = false;
    unsigned steady_stage_ = 0, steady_slot_ = Count; std::uint64_t steady_fence_ = 0; std::int64_t steady_pts_ = 0;
    std::uint32_t steady_surface_ = 0, steady_epoch_ = UINT32_MAX, steadied_ = 0, steady_grid_width_ = 0, steady_grid_height_ = 0, steady_grid_words_ = 0;
    struct SteadyJob { const std::uint8_t* cells; unsigned stride, width, height; std::int64_t pts; float zoom, x, y; bool restart; };
    struct SteadyResult { SteadyView view{}; float dx = 0, dy = 0, turn = 0; bool valid = false; };
    SteadyJob steady_job_data_{}; SteadyResult steady_result_{};
    std::atomic<int> steady_job_{0}; std::atomic<HANDLE> steady_wake_{nullptr}; std::atomic<std::uint32_t> steady_measured_{0};
    float steady_mean_ = -1; std::int64_t steady_newest_ = INT64_MIN; bool steady_reversed_ = false; // The thread's.
    SteadyView held_steady_{};           // The view the held picture was converted on.
    // The motion for the event camera since the grid it last took (Slot::motion_*), and what goes with the next one.
    float sense_x_ = 0, sense_y_ = 0, sense_turn_ = 0; float sense_given_x_ = 0, sense_given_y_ = 0, sense_given_turn_ = 0; bool sense_measured_ = false;
    // The region vibrometry reads in a picture converted now (crop samples), and whether it follows the target; false: off.
    bool VibroRegion(const core::VideoFrameDescriptor& frame, video::AreaRect& region, bool& follow) noexcept;
    void HarvestVibro(std::uint64_t completed) noexcept;
    // The event camera (2026-11-03): its sensor, the virtual IMU on the grids, the request a host's thread made (packed: its
    // sequence, on, stabilised, the events a pixel may fire, the threshold's bits) and the presentation thread's state.
    static constexpr unsigned kEventRing = 1u << 15; // Events (256 KiB): a host polls them every picture (docs/picture.md, "The telemetry module's memory").
    video::EventCamera events_;
    video::VirtualImu grid_imu_;
    std::atomic<std::uint64_t> events_request_{0};
    std::uint32_t events_applied_ = 0, events_epoch_ = 0;
    bool events_on_ = false;
    // The presentation thread's side (the request applied: options to hand over), and what goes with a grid to the thread.
    video::EventCameraOptions events_options_{}; bool events_restart_ = false;
    bool perceive_run_ = false, sense_run_ = false, sense_restart_ = false; std::uint32_t sense_epoch_ = 0;
    video::EventCameraOptions sense_options_{};
    std::atomic<std::uint32_t> events_skipped_{0};
    std::atomic<std::uint64_t> events_emitted_{0}, events_dropped_{0};
    std::atomic<std::uint32_t> events_pictures_{0}, events_rebases_{0}, events_last_{0};
    void ApplyEventRequest() noexcept;
    // Tensor export (2026-11-03): the cells' buffer and readback (mapped), the request and its answer, its conversion's fence.
    // State: 0 idle, 3 the asker sets it up, 1 asked, 4 the presentation thread records it (with the next conversion), 2
    // recorded (its fence: tensor_fence_), 5 refused (a region off the picture).
    ID3D12Resource* tensor_buffer_ = nullptr; ID3D12Resource* tensor_readback_ = nullptr; const float* tensor_data_ = nullptr;
    std::uint64_t tensor_cells_ = 0;
    std::atomic<std::uint32_t> tensor_state_{0};
    TensorAsk tensor_ask_{}; TensorAnswer tensor_answer_{};
    std::atomic<std::uint64_t> tensor_fence_{0};
    std::atomic<bool> tensor_used_{false}; // A host has exported one: a pause gets the source ready for the next.
    bool present_paused_ = false;          // The last presentation pass's state.
    bool TensorRegion(const core::VideoFrameDescriptor& frame, const SteadyView& steady, video::AreaRect& region) const noexcept; // Crop samples.
    RenderStatus HeldTensor() noexcept;    // A request while paused: from the held picture, or by a refresh landing.
    void Answer(const core::VideoFrameDescriptor& frame, const video::AreaRect& region, std::uint32_t transfer_id, bool held) noexcept;
    void Sense(const std::uint8_t* cells, unsigned width, unsigned height, std::int64_t pts, std::uint32_t epoch) noexcept; // Perception's thread.
    std::int64_t readout_pts_ = INT64_MIN; std::uint32_t readouts_ = 0; char readout_[64]{};
    void Follow(float x, float y) noexcept;             // The view's centre moves (the tracker), no cue and no live-zoom record.
    std::int64_t cuts_[kCutRing]{};
    unsigned cut_count_ = 0, luma_pictures_ = 0;
    float last_luma_ = 0;
    std::uint32_t motion_epoch_ = UINT32_MAX; Region motion_region_{}; // The statistics' previous picture (block motion).
    unsigned tier_frames_[kQualityTiers]{}, load_repeats_ = 0;
    float test_load_ = 0;
    const subtitle::SubtitleTrack* subtitle_track_ = nullptr;
    std::atomic<const subtitle::SubtitleTrack*> pending_rebind_{nullptr}; // API 17: asked by any thread, taken by the presentation thread.
    std::atomic<bool> subtitles_prepared_{false};
    unsigned subtitle_presents_ = 0, cues_shown_ = 0;
    std::uint64_t cue_seen_[subtitle::SubtitleTrack::kMaxCues / 64]{};
    std::uint64_t subtitle_stamp_ = 0, staged_subtitle_stamp_ = 0;
    SubtitleFrame staged_subtitles_{}, shown_subtitles_{};
    ui::KineticHud hud_;
    ui::TimelineMarks marks_{};
    ui::BookmarkMarks bookmark_marks_{};
    // The controls as the last image showed them, and as the staged one draws them (HudChanged).
    ui::HudUniforms drawn_hud_{}, staged_hud_uniforms_{};
    ui::TimelineMarks drawn_marks_{}, staged_marks_{};
    ui::BookmarkMarks drawn_bookmarks_{}, staged_bookmarks_{};
    bool HudChanged() const noexcept;
    ui::CommandPalette palette_;
    PalettePipeline& palette_pipeline_ = gpu_.palette;
    PaletteSubmit palette_submit_ = nullptr;
    PalettePreview palette_preview_ = nullptr;
    void* palette_context_ = nullptr;
    std::uint32_t previewed_ = 0; // Palette revision the hint was computed for.
    unsigned palette_presents_ = 0, palette_submits_ = 0, palette_rejections_ = 0;
    bool staged_palette_ = false, palette_was_drawn_ = false;
    ui::StatusCue status_;
    StatusPipeline& status_pipeline_ = gpu_.status;
    unsigned status_presents_ = 0, status_shown_ = 0;
    std::uint32_t status_counted_ = 0; // The last revision counted in status_shown_.
    bool staged_status_ = false, status_was_drawn_ = false;
    HudHandler hud_handler_ = nullptr;
    KeyHandler key_handler_ = nullptr;
    void* key_context_ = nullptr;
    void* hud_context_ = nullptr;
    ID3D12DescriptorHeap*& rtvs_ = gpu_.rtvs;
    ID3D12RootSignature*& blit_root_ = gpu_.blit_root; // Scaled composition of a picture converted for another window size.
    ID3D12PipelineState*& blit_ = gpu_.blit;
    unsigned scaled_composites_ = 0;
    ID3D12Resource* history_ = nullptr;
    // Paused re-conversion: the source of a landing converted while paused (the decode surfaces' description), its frame,
    // and the list that converts it again into history_.
    ID3D12Resource* held_ = nullptr;
    core::VideoFrameDescriptor held_frame_{};
    ID3D12CommandAllocator* still_allocator_ = nullptr;
    ID3D12GraphicsCommandList* still_commands_ = nullptr;
    std::uint64_t still_fence_ = 0;
    bool held_valid_ = false, reconvert_pending_ = false, refresh_wanted_ = false, refresh_in_flight_ = false, reconvert_after_refresh_ = false;
    bool reconverted_ = false, reconverted_staged_ = false;
    std::int64_t settings_asked_ = 0;
    PausedUpdate paused_updates_[kMaxPausedUpdates]{}; unsigned paused_update_count_ = 0; PausedUpdate pending_update_{};
    unsigned awaiting_update_ = kMaxPausedUpdates; std::uint64_t awaiting_fence_ = 0; // The update whose image the GPU has not finished.
    ID3D12Resource* capture_readback_ = nullptr;
    std::uint64_t capture_fence_ = 0;
    bool capture_enabled_ = false, capture_requested_ = false, capture_pending_ = false, capture_horizon_ = false, capture_spots_ = false;
    Spot spots_[kMaxSpots]{}; unsigned spot_count_ = 0; std::int64_t capture_pts_ = 0;
    Capture captures_[kMaxCaptures]{}; unsigned capture_count_ = 0, capture_width_ = 0, capture_height_ = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT capture_layout_{};
    CompositeContext (&composites_)[3] = gpu_.composites;
    ID3D12DescriptorHeap* descriptors_ = nullptr;
    ID3D12Resource* (&buffers_)[3] = gpu_.buffers;
    Slot slots_[Count]{};
    video::ReorderWindow<Count> order_;
    HANDLE& latency_ = gpu_.latency;
    HANDLE& event_ = gpu_.event;
    // Test posts admitted through isolation: written by the posting thread, taken by the thread that pumps the window.
    struct TaggedInput { std::atomic<bool> armed{false}; UINT message = 0; WPARAM w = 0; LPARAM l = 0; };
    TaggedInput tagged_[16]{};
    std::atomic<unsigned> isolated_inputs_{0};
    std::atomic<bool> isolate_input_{false};
    // The window thread (SetMessageThread): the input ring (the window thread produces, the presentation thread consumes).
    struct InputEvent { UINT message; WPARAM w; LPARAM l; std::int64_t time; std::uint32_t keys; std::uint32_t lengths[2]; wchar_t text[2][64]; };
    core::SpscRingBuffer<InputEvent, 64> input_;
    HANDLE input_wake_ = nullptr;
    bool message_thread_ = false;
    HWND parent_ = nullptr;                          // Embedding (API 12): the host's window, or null.
    // Texture output (API 13, 14): the shared textures and fences (render::SharedOutput), their format.
    bool texture_output_ = false;
    unsigned output_width_ = 0, output_height_ = 0;
    SharedOutput::Format output_format_ = SharedOutput::Format::Rgba8;
    SharedOutput output_;
    std::atomic<std::uint64_t> output_request_{0};   // A size the host asked for (width << 32 | height); 0: none.
    bool CreateOutputs(unsigned width, unsigned height) noexcept;
    void ReleaseOutputs() noexcept;
    HWINEVENTHOOK parent_hook_ = nullptr;            // Its location hook, on the child's thread (unhooked at WM_DESTROY there).
    std::atomic<std::uint32_t> fullscreen_requests_{0};
    std::uint32_t keys_ = 0;                 // The modifiers of the message being handled (Down).
    mutable InputStats input_stats_{};
    std::int64_t input_since_ = 0;
    std::atomic<std::uint32_t> input_lost_{0}; // Input events dropped: the ring was full.
    bool ui_tracking_ = false, ui_captured_ = false; // Window thread only.
    std::atomic<bool> ui_fullscreen_{false}, ui_palette_{false};
    std::atomic<unsigned> ui_dpi_{96};
    unsigned capacity_width_ = 0, capacity_height_ = 0, history_width_ = 0, history_height_ = 0;
    unsigned pending_width_ = 0, pending_height_ = 0, resizes_ = 0;
    float dpi_ = 1;
    video::SampleAspect sample_aspect_{}; // Normalized; 1:1 when the stream's samples are square.
    bool resize_pending_ = false, redraw_ = false, fullscreen_ = false;
    RECT restore_{};
    WindowHandoff adopt_{}, handoff_{};
    bool keep_window_ = false, adopted_ = false;
    unsigned kept_slots_ = 0;
    VideoRect video_rect_{};
    PumpHandler pump_ = nullptr;
    void* pump_context_ = nullptr;
    unsigned width_ = 0, height_ = 0, surfaces_ = 0, stride_ = 0, presented_ = 0, dropped_ = 0, out_of_order_ = 0, tone_mapped_ = 0;
    std::uint64_t converted_ = 0;
    std::uint64_t& sequence_ = gpu_.sequence;
    std::int64_t input_frequency_ = 0, hud_advance_ = 0, hud_present_ = 0, hud_media_ = 0;
    std::int64_t untimed_since_ = 0; // Playing, a picture in order waiting for the clock since (0: none); see Present.
    bool awaiting_clock_ = true, was_paused_ = false; // The start or a resume, until its first timed picture; see Present.
    Wake wake_{}; // NextWake.
    std::int64_t settled_wait_ = 0; bool settled_ = false, warm_perception_ = true;
    unsigned& rtv_stride_ = gpu_.rtv_stride;
    unsigned staged_slot_ = Count, staged_context_ = 0;
    unsigned hud_presents_ = 0, paused_hud_presents_ = 0, rejected_seeks_ = 0, stale_ = 0;
    std::uint32_t epoch_ = 1, ready_epoch_ = 0;
    Drop last_drop_{};
    OrderReject last_reject_{};
    std::int64_t staged_deadline_ = 0, refresh_period_ = 166667;
    LateFlip worst_flip_{};
    std::uint8_t waited_ = 0;
    TraceConversion trace_conversions_[kTrace]{};
    TracePicture trace_pictures_[kTrace]{};
    unsigned trace_converted_ = 0, trace_pictured_ = 0;
    std::int64_t trace_seen_pts_ = INT64_MIN, trace_seen_ = 0, trace_decoded_ = 0; // The picture ConvertFrame holds back.
    void Trace(std::int64_t pts, std::int64_t deadline, std::int64_t at, bool dropped) noexcept {
        if (trace_pictured_ < kTrace) trace_pictures_[trace_pictured_++] = {pts, deadline, at, waited_, dropped};
    }
    std::int64_t seek_requested_ = 0, seek_target_ = INT64_MIN;
    unsigned volume_presents_ = 0;
    int wheel_ = 0; // Wheel delta not yet amounting to a notch (high-resolution wheels and touchpads).
    SeekPresentation seek_{};
    // The first picture of every epoch, including the initial one, is shown as soon as it is in order: before the clock
    // has a deadline for it. Otherwise a clock that anchors more than one frame after start would drop it.
    bool preview_ = true;
    enum class Adoption : std::uint8_t { None, Pending, Confirmed } adoption_ = Adoption::None;
    std::uint32_t adopt_chain_ = 0;  // Earliest epoch whose pictures are adopted.
    std::int64_t adopt_floor_ = 0;   // Adopted pictures must end after this (the latest queue seek's target).
    unsigned adopted_frames_ = 0;
    // HDR dynamic peak in presentation time (docs/picture.md, "HDR peak per epoch"): restarted by every epoch that is not
    // a queue seek, so a seek's landing is tone-mapped exactly as the same picture decoded cold.
    PeakTiming peak_timing_;
    bool recovery_preview_ = false; // Device recovered: the next frame of the current epoch is shown at once.
    unsigned recoveries_ = 0, video_width_ = 0, video_height_ = 0;
    std::int64_t health_at_ = 0, recovery_requested_ = 0, shown_pts_ = INT64_MIN, shown_duration_ = 0;
    std::uint32_t shown_transfer_ = 0; // The H.273 transfer of the picture on screen (texture output's frames carry it).
    float zoom_ = 1.0f, zoom_x_ = 0.5f, zoom_y_ = 0.5f; // The zoom and its centre (fractions of the crop).
    Region last_region_{};
    // Live zoom: the history's crop and converted region, the zoom generation (one per change) and what each composite
    // staged; the record of changes made while playing.
    Region history_crop_{}, history_region_{};
    std::uint32_t zoom_generation_ = 0, history_generation_ = 0, staged_generation_ = 0;
    bool staged_native_ = false, staged_transformed_ = false, zoom_redraw_ = false;
    // Comparisons and the reference frame; the pinned loop pictures; their copy list.
    Compare compare_mode_ = Compare::Off;
    ID3D12Resource* reference_ = nullptr;
    bool reference_valid_ = false;
    unsigned reference_width_ = 0, reference_height_ = 0, pinned_compare_composites_ = 0;
    std::int64_t reference_pts_ = INT64_MIN;
    struct Pin { core::VideoFrameDescriptor frame{}; unsigned width = 0, height = 0; Region crop{}, region{}; std::uint32_t zoom_generation = 0; };
    ID3D12Resource* pin_rgb_[kPins]{};
    Pin pins_[kPins]{};
    unsigned pin_count_ = 0, pinned_turns_ = 0;
    bool pin_armed_ = false, pin_complete_ = false;
    std::int64_t pin_a_ = 0, pin_b_ = 0;
    std::uint32_t pin_epoch_ = 0;
    ID3D12CommandAllocator* pin_allocator_ = nullptr;      // Direct: the reference frame's copy.
    ID3D12GraphicsCommandList* pin_commands_ = nullptr;
    std::uint64_t pin_fence_ = 0;
    ID3D12CommandQueue* copy_queue_ = nullptr;             // A turn's copies: the landing's alone, then the others.
    ID3D12Fence* copy_fence_ = nullptr;
    ID3D12CommandAllocator* turn_allocators_[2]{};
    ID3D12GraphicsCommandList* turn_commands_[2]{};
    std::uint64_t copy_sequence_ = 0;
    std::uint64_t turn_fence_[2]{};           // Each turn list's last copy (its allocator may be reset once it has run).
    // The pinned turn's landing staged ahead of B (StageLoopLanding, 2026-11-02): the slot held for it (Count: none), the
    // timeline and pin it was made for; landings staged, turns that took one.
    unsigned landing_slot_ = Count; std::uint32_t landing_epoch_ = 0; std::int64_t landing_pts_ = 0;
    std::uint32_t staged_landings_ = 0, staged_turns_ = 0;
    void ReleaseStagedLanding() noexcept;
    bool SlotReady(const Slot& s, std::uint64_t completed) const noexcept {
        return completed >= s.fence && (!s.copy_fence || copy_fence_->GetCompletedValue() >= s.copy_fence);
    }
    bool screen_capture_requested_ = false, reference_capture_requested_ = false;
    CaptureKind capture_kind_ = CaptureKind::Held;
    void PinPicture(const Slot&, ID3D12GraphicsCommandList*) noexcept; // Composite: the loop pins' capture.
    // Cold start: the warm-up flip (WarmUp).
    bool fresh_swap_ = false;
    std::int64_t warm_up_ = 0, warm_up_present_ = 0;
    HRESULT warm_up_result_ = S_OK;
    bool WarmUpFlip() noexcept;
    LiveZoom live_zooms_[kMaxLiveZooms]{};
    unsigned live_zoom_count_ = 0, transformed_composites_ = 0;
    void ZoomCue() noexcept;
    SeekPresentation recovery_landing_{};
    std::uint32_t reconstruction_ = kEdgeDirected;
    float sharpness_ = 0.0f; // Opt-in: about 3 ms per 1080p frame on an Intel UHD 630.
    PictureControls picture_{};
    bool hud_enabled_ = true, neural_allowed_ = true;
    // Micro-motion's reference: when it starts over, and the last picture folded into it (presentation order).
    bool micro_start_ = true; std::int64_t micro_pts_ = INT64_MIN; std::uint32_t micro_epoch_ = 0;
    video::Orientation orientation_{}; unsigned rotations_ = 0, turn_generation_ = 0, micro_turn_ = 0;
    bool auto_level_ = false; video::HorizonLevel horizon_{}; unsigned horizon_reads_ = 0;
    void FollowHorizon() noexcept; // The leveling turn to the horizon's tilt (auto-leveling).
    bool auto_crop_ = false; video::Matte matte_{}; std::uint32_t cropped_changes_ = 0; // The matte's Changes() the view last followed.
    std::uint32_t matte_pictures_ = 0; // Conversions since the bars were known (kMatte on every fourth).
    void FollowBars() noexcept;    // The view to the active area (auto-crop).
    unsigned micro_width_ = 0, micro_height_ = 0, micro_pictures_ = 0; Region micro_region_{};
    SignatureSink signature_sink_ = nullptr; void* signature_context_ = nullptr;
    bool history_valid_ = false, swap_pending_ = false, hud_was_drawn_ = false, staged_hud_ = false;
    bool hud_paused_ = false, tracking_mouse_ = false, captured_ = false;
    HRESULT error_ = S_OK;
    std::atomic<bool> closed_{false}; // Set by the window thread too (WM_CLOSE).
    bool bound_ = false, ten_bit_ = false;
};
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
