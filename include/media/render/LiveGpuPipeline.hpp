#pragma once
#include "media/render/ColorPipeline.hpp"
#include "media/render/NeuralUpscalePass.hpp"
#include <d3d12.h>
#include <cstdint>

namespace media::render {
// GPU live ingest (docs/picture.md, "GPU live ingest"; the C SDK's MEDIA_SENSOR_GPU): a live camera's pictures, pushed by the
// host, through the windowed engine's own GPU passes, offscreen: the upload into a pre-allocated NV12 (or P010) texture ring,
// then the view's region (zoom, pan, the audio saccade, tracking, stabilisation: the host engine's view) converted by
// ColorPipeline to an RGBA image of the output size, with the picture controls (shadow lift and the other four), micro-motion,
// and edge-directed or neural (NeuralUpscalePass) super-resolution while upscaling; the image is copied to a mapped readback
// buffer for the host (a robot's display, an encoder, a network).
//
// Everything is made by Create (a device on the first hardware adapter, one direct queue, kRing slots of upload buffer,
// source texture, RGBA image and readback, the neural pass and its 2x luma, micro-motion's double reference, timestamps);
// Ingest writes into the slot's mapped upload buffer, records and submits one list, and never waits: when every slot's work
// is still on the GPU the picture is refused (Busy, counted). One thread.
//
// 2026-10-27 (docs/picture.md, "Roll and compensated micro-motion", "Ten-bit live ingest"):
// - Ten-bit: created with `ten_bit`, the texture ring is P010 and IngestP010 uploads a camera's P010 samples as they are (all
//   1,024 levels, the conversion's 10-bit path and its dither); 8-bit pictures go into it as their 10-bit codes (v x 4).
// - The view turns (LiveView::roll, the stabiliser's) and moves by sub-texel amounts: the crop is on even texels, the rest of
//   the view's position and its turn go to the conversion (ColorConstants kRotate). Neural reconstruction is left out then
//   (its 2x image is of the upright crop): edge-directed.
// - Micro-motion follows the view: its reference is kept per output pixel, so when the view moves (stabilisation, a saccade,
//   tracking) each pixel reads it where its content was in the previous picture's output (kMicroWarp): the previous view's
//   mapping, the scene's own motion from the IMU (LiveView::scene_*), this view's mapping, as one affine map. It starts over
//   only when the view's size changes (a zoom), on LiveView::restart, when turned on, and per pixel for content from outside
//   the previous view; a cut changes every pixel by more than micro-motion's motion threshold, which restarts each of them.
struct LiveControls {
    float brightness = 0, contrast = 1, saturation = 1, gamma = 1, shadow_lift = 0, micro_motion = 1; // As D3D12Renderer::PictureControls.
    std::uint32_t reconstruction = kEdgeDirected; // kBilinear, kEdgeDirected or kNeural (the 2x luma network) while upscaling.
};
struct LiveView {
    float zoom = 1, x = 0.5f, y = 0.5f; // The view's centre, fractions of the picture.
    float roll = 0;                     // The view's turn about its centre, radians, picture coordinates (+: clockwise on screen).
    // How the scene moved in the picture since the previous picture (the IMU's rotation): by (scene_dx, scene_dy) pixels at
    // the centre and turned by scene_roll about it (radians, picture coordinates). 0: still, or unknown.
    float scene_dx = 0, scene_dy = 0, scene_roll = 0;
    bool restart = false;               // Micro-motion starts over (a cut the host knows of).
    // How the image shows the view (2026-10-31, API 7): display = D stored about the centres, D = (d[0] d[1]; d[2] d[3]) a
    // quarter turn or mirror of the axes (video::Orientation::Display). An odd turn converts a region of the output's turned
    // shape (its height : its width), as large as the view holds, so that the turned picture fills the image undistorted.
    int display[4]{1, 0, 0, 1};
    // Fit instead (2026-11-01, MEDIA_SENSOR_GPU_FIT): the view's whole region, turned, scaled to fit inside the image (one scale
    // both ways, centred), black bars where it does not reach: no sample of the view is lost.
    bool fit = false;
};
struct LiveImage {
    std::int64_t pts_100ns = -1;
    std::uint32_t sequence = 0, width = 0, height = 0, stride = 0; // stride: bytes per row of `rgba`.
    const std::uint8_t* rgba = nullptr;  // Valid until kRing - 1 more pictures have been ingested.
    std::uint32_t left = 0, top = 0, right = 0, bottom = 0; // The camera's region converted (the turned view lies around it).
    std::uint32_t reconstruction = 0, picture_flags = 0;    // As converted (kNeural only while upscaling; kGrade, kMicro, kMicroWarp).
    double gpu_ms = -1;                                      // The upload and the conversion on the GPU (timestamps; -1 unknown).
    float roll = 0;                                          // The view's turn, as converted.
    bool ten_bit = false;                                    // Converted from 10-bit samples.
    int display[4]{1, 0, 0, 1};                              // LiveView::display, as converted.
    // LiveView::fit with bars (2026-11-01): the picture lies in [picture_left, picture_right) x [picture_top, picture_bottom) of
    // the image (pixels), black around it; the whole image otherwise.
    bool fitted = false;
    std::uint32_t picture_left = 0, picture_top = 0, picture_right = 0, picture_bottom = 0;
};
class LiveGpuPipeline final {
public:
    static constexpr unsigned kRing = 3;
    LiveGpuPipeline() noexcept = default;
    LiveGpuPipeline(const LiveGpuPipeline&) = delete;
    LiveGpuPipeline& operator=(const LiveGpuPipeline&) = delete;
    ~LiveGpuPipeline() noexcept { Release(); }
    // Setup only. `width` x `height`: the camera (even, at least 64 x 64); `out_width` x `out_height`: the image (even; larger
    // than the region is super-resolution); `ten_bit`: P010 textures (IngestP010). False: no hardware D3D12 device, or a
    // resource could not be made (LastError()).
    [[nodiscard]] bool Create(unsigned width, unsigned height, unsigned out_width, unsigned out_height, bool ten_bit = false) noexcept;
    [[nodiscard]] bool Ready() const noexcept { return queue_ != nullptr; }
    [[nodiscard]] bool TenBit() const noexcept { return ten_bit_; }
    void Release() noexcept;
    // From the next picture (clamped as the windowed engine clamps them). Micro-motion's reference starts over when it is
    // turned on.
    void SetControls(const LiveControls&) noexcept;
    [[nodiscard]] const LiveControls& Controls() const noexcept { return controls_; }
    enum class Status : std::uint8_t { Ready, Busy, Failed };
    // One 8-bit 4:2:0 picture as three planes (Cb and Cr each half the size each way); interleaved into the upload as NV12
    // (as 10-bit codes into a ten-bit ring).
    Status Ingest(const std::uint8_t* luma, unsigned luma_stride, const std::uint8_t* cb, const std::uint8_t* cr, unsigned chroma_stride,
                  std::int64_t pts_100ns, const LiveView& view) noexcept;
    // One P010 picture as the camera gives it (16-bit little-endian samples, 10 bits in the high bits; a luma plane and an
    // interleaved CbCr plane, strides in bytes). Failed unless created ten-bit.
    Status IngestP010(const std::uint8_t* luma, unsigned luma_stride, const std::uint8_t* cbcr, unsigned cbcr_stride, std::int64_t pts_100ns,
                      const LiveView& view) noexcept;
    // The newest image whose GPU work has completed (no wait). False: none yet.
    bool Latest(LiveImage& image) noexcept;
    // Waits for every submitted picture (tests, shutdown). False on a timeout or a lost device.
    bool Drain(unsigned milliseconds) noexcept;
    struct Stats { std::uint32_t ingested = 0, busy = 0, neural = 0, micro = 0, completed = 0, warped = 0, restarts = 0, turned = 0; double gpu_ms_max = 0; };
    [[nodiscard]] const Stats& Statistics() const noexcept { return stats_; }
    [[nodiscard]] HRESULT LastError() const noexcept { return error_; }
private:
    struct Slot {
        ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Resource *upload = nullptr, *source = nullptr, *image = nullptr, *readback = nullptr;
        std::uint8_t *upload_data = nullptr, *readback_data = nullptr;
        std::uint64_t fence = 0; LiveImage info{};
        bool busy = false;
    };
    // The view's mapping from output pixel centres X to camera pixels: crop origin + c + R (U scale - c) + d, where U = A X + b
    // undoes the display's quarter turns and mirror (A = D's transpose; the identity upright).
    struct Mapping {
        double left = 0, top = 0, cx = 0, cy = 0, sx = 1, sy = 1, cos = 1, sin = 0, dx = 0, dy = 0;
        double a[4]{1, 0, 0, 1}, b[2]{};
        void Forward(double x, double y, double& cx_out, double& cy_out) const noexcept;  // Output -> camera.
        void Inverse(double qx, double qy, double& x_out, double& y_out) const noexcept;  // Camera -> output.
    };
    void Region(const LiveView&, std::uint32_t& left, std::uint32_t& top, std::uint32_t& right, std::uint32_t& bottom) const noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned i) const noexcept;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(unsigned i) const noexcept;
    void Collect() noexcept; // Completed slots: their GPU time, Latest's candidate.
    Slot* Begin() noexcept;  // The next free slot (nullptr: busy, counted, or failed).
    Status Submit(Slot&, std::int64_t pts, const LiveView& view) noexcept; // The upload buffer is filled: record and submit.

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    ID3D12Resource* neural_texture_ = nullptr;
    ID3D12QueryHeap* timestamps_ = nullptr;
    ID3D12Resource* timestamp_readback_ = nullptr; std::uint64_t* timestamp_data_ = nullptr;
    HANDLE wait_ = nullptr;
    ColorPipeline color_;
    NeuralUpscalePass neural_;
    Slot slots_[kRing]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT planes_[2]{}, image_footprint_{};
    unsigned width_ = 0, height_ = 0, out_width_ = 0, out_height_ = 0, stride_ = 0, next_ = 0;
    std::uint64_t sequence_ = 0, ticks_ = 0;
    int latest_ = -1; // The slot Latest returns.
    bool ten_bit_ = false;
    LiveControls controls_{};
    bool micro_start_ = true; std::uint32_t micro_size_[2]{}; unsigned micro_phase_ = 0; // Which half of the reference is read.
    Mapping previous_{}; bool previous_valid_ = false; // The previous micro-motion picture's view.
    Stats stats_{};
    HRESULT error_ = S_OK;
};
}
