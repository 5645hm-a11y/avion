#pragma once
#include <d3d12.h>
#include <cstdint>

namespace media::render {
// Root constants shared by every colour-conversion pass (44 x 32-bit). Zero-filled trailing fields select the
// original bilinear, unsharpened conversion, without picture controls or micro-motion.
struct ColorConstants {
    std::uint32_t width, height, coded_width, coded_height;
    std::uint32_t left, top, right, bottom;  // Crop in coded pixels, right/bottom exclusive.
    std::uint32_t matrix, full_range, ten_bit, primaries;
    std::uint32_t transfer;                  // H.273 transfer_characteristics; 16 = PQ, 18 = HLG.
    std::uint32_t peak_mode;                 // HDR: 0 = GPU-detected dynamic peak, 1 = fixed_peak_nits.
    float fixed_peak_nits, target_nits;      // target_nits: HDR level shown as SDR white (BT.2408: 203).
    std::uint32_t reconstruction;            // 0 = bilinear; 1 = edge-directed luma while upscaling; 2 = neural 2x luma
                                             // (NeuralUpscalePass output bound as t2) while upscaling (Reconstruct.hlsli).
    float sharpness;                         // Contrast-adaptive sharpening in linear light, 0 (off) to 1.
    float noise_floor;                       // Local contrast (encoded units) below which sharpening fades out.
    std::uint32_t chroma_mode;               // kGuidedChroma: luma-guided 4:2:0 chroma upsampling (Reconstruct.hlsli).
    // HDR dynamic peak (peak_mode 0): presentation time since the last picture folded into the smoothed peak (s; 0 for a
    // picture presenting before one already folded), and kPeakStart for the first picture of an epoch (PeakTiming).
    float peak_dt;
    std::uint32_t peak_flags;
    // docs/picture.md, "Downscale prefilter and dither": kAreaPrefilter averages each output pixel's source footprint
    // while downscaling (instead of one bilinear sample); kDither adds a static +-0.5 LSB interleaved-gradient-noise
    // dither to the 8-bit output of 10-bit and HDR sources. 0 reproduces the conversion before them (reference tests).
    // kCompare splits the picture (docs/picture.md, "Original | Enhanced"): the left half converted plain (bilinear luma
    // and chroma, no sharpening), the right half as configured, a two-pixel seam between them.
    std::uint32_t filter_flags;
    // docs/picture.md, "Picture controls and micro-motion" (Reconstruct.hlsli): kGrade applies the five controls below (each
    // only when not neutral); kMicro magnifies small temporal luma changes by micro_gain against the per-pixel reference
    // (ColorPipeline::PrepareMotion), kMicroStart starts it over, kMicroFold moves it toward this picture.
    std::uint32_t picture_flags;
    float brightness, contrast, saturation, gamma, shadow_lift, micro_gain;
    // docs/picture.md, "Roll and compensated micro-motion", "Rotation" (2026-10-27/28; Reconstruct.hlsli): kRotate (filter_flags)
    // maps each output pixel's centre X through view_m and view_t, q = (m0 X + m1 Y + t0, m2 X + m3 Y + t1) luma texels from the
    // crop's origin, instead of scaling: a turn, a quarter turn or mirror (a file's tkhd), a sub-texel move. Reads clamp to the
    // crop, or with kSurfaceClamp to the coded surface (live ingest: no padding, a turned view's corners reach past the crop).
    // It selects the PICTURE_CONTROLS variants. Micro-motion's reference is
    // read at micro_read and written at micro_write (floats into its buffer; 0, 0: in place); kMicroWarp reads it where the
    // pixel's content was in the previous picture's output, micro_map (m0 X + m1 Y + m2, m3 X + m4 Y + m5) for the output
    // pixel's centre, which needs micro_read != micro_write.
    float view_m[4], view_t[2];
    std::uint32_t micro_read, micro_write;
    float micro_map[6];
};
static_assert(sizeof(ColorConstants) == 176);
// kHorizon (2026-10-29) is the statistics pass's: the edge-orientation histograms for auto-leveling (RecordStats); so is kMatte
// (2026-10-30): the picture's extents for video::Matte (without it those words stay zero, which says nothing about bars). The
// conversion ignores both. kBars (2026-11-01, with kRotate): an output pixel whose centre maps outside the crop is black (live
// ingest's fit: the turned picture whole inside the image, bars around it).
// kHdrOutput (API 14, texture output in an HDR format; Reconstruct.hlsli): the picture is written as PQ BT.2020 (an HDR10 signal)
// instead of display-referred BT.709: PQ and HLG pictures are not tone-mapped (HLG through its 1000 cd/m2 OOTF), SDR pictures land
// at reference white (target_nits, 203); with kLinearOutput too, the PQ value is written as scRGB (linear BT.709, 1.0 = 80 cd/m2,
// negative outside BT.709's gamut) for an FP16 texture. The rest of the pass works on the PQ code values as it does on gamma ones.
enum : std::uint32_t { kAreaPrefilter = 1, kDither = 2, kCompare = 4, kRotate = 8, kSurfaceClamp = 16, kHorizon = 32, kMatte = 64, kBars = 128,
                       kHdrOutput = 256, kLinearOutput = 512 };
enum : std::uint32_t { kGrade = 1, kMicro = 2, kMicroStart = 4, kMicroFold = 8, kMicroWarp = 16 };
enum : std::uint32_t { kPeakStart = 1 };
// The presentation-time step of the dynamic peak (renderer, per HDR conversion): pictures arrive in decode order, so a
// picture that presents before one already folded adds no time; a new epoch starts over (no inherited exposure).
struct PeakTiming {
    std::int64_t last = INT64_MIN;
    bool start = true;
    void Restart() noexcept { start = true; last = INT64_MIN; }
    // Seconds to fold for a picture at `pts` (100 ns), and kPeakStart in `flags` for the first of an epoch.
    float Step(std::int64_t pts, std::uint32_t& flags) noexcept {
        flags = start ? kPeakStart : 0u;
        if (start) { start = false; last = pts; return 0.0f; }
        if (pts <= last) return 0.0f;
        const auto dt = pts - last; last = pts;
        return static_cast<float>(static_cast<double>(dt) / 1e7);
    }
};
enum : std::uint32_t { kBilinear = 0, kEdgeDirected = 1, kNeural = 2 };
enum : std::uint32_t { kBilinearChroma = 0, kGuidedChroma = 1 };
[[nodiscard]] constexpr bool IsHdrTransfer(std::uint32_t transfer) noexcept { return transfer == 16 || transfer == 18; }

// Planar 4:2:0 (NV12/P010) to display RGB on the compute queue. The caller binds a shader-visible heap with
// two plane SRVs (luma, chroma) and one RGBA8 UAV. SDR streams use the matrix/primaries pass; PQ and HLG use
// the dedicated tone-mapping pass, whose source peak is tracked on the GPU (no CPU readback).
class ColorPipeline final {
public:
    ColorPipeline() noexcept = default;
    ColorPipeline(const ColorPipeline&) = delete;
    ColorPipeline& operator=(const ColorPipeline&) = delete;
    ~ColorPipeline() noexcept { Release(); }
    [[nodiscard]] HRESULT Create(ID3D12Device*) noexcept;
    // The conversion's bindings alone, for the passes recorded without one (RecordGrid, RecordTensor).
    void Bind(ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE planes, D3D12_GPU_DESCRIPTOR_HANDLE target, const ColorConstants&,
              D3D12_GPU_DESCRIPTOR_HANDLE neural = {}) const noexcept;
    // `neural`: SRV of the 2x luma for reconstruction kNeural (ignored otherwise; 0 binds the planes in its place).
    void Record(ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE planes, D3D12_GPU_DESCRIPTOR_HANDLE target,
                const ColorConstants&, D3D12_GPU_DESCRIPTOR_HANDLE neural = {}) const noexcept;
    // Scene statistics (docs/chapters.md, "Live cuts"), after Record in the same list, with its bindings still set: the
    // picture's luma sum and sample count are added to running totals ([32], [36]), its brightest sample ([40]) and moving
    // 8x8 blocks ([44]) are found (docs/sdk.md, "Frame signatures"), and the picture's extents for video::Matte ([48, 64):
    // docs/picture.md, "Black bars"), and the state from [8] to [64) (pictures, cuts, epoch starts, luma totals, peak, moving
    // blocks, extents) is copied to `readback` at `offset` for the CPU once the list has run; the per-picture fields are then
    // cleared.
    // With kHorizon in the constants' filter_flags, the picture's edge-orientation histograms (HdrToneMap.hlsl Orientation,
    // video::HorizonLevel's input: kHorizonBytes) are made by the same pass and copied to `horizon` at `horizon_offset`.
    void RecordStats(ID3D12GraphicsCommandList*, const ColorConstants&, ID3D12Resource* readback, std::uint64_t offset, ID3D12Resource* horizon = nullptr,
                     std::uint64_t horizon_offset = 0) const noexcept;
    static constexpr std::uint32_t kStatsBytes = 56, kHorizonBytes = 1024;
    // Target tracking's luma grid (HdrToneMap.hlsl Grid) of the crop [left, right) x [top, bottom) of `constants` at `scale`,
    // `words` words a row and `rows` rows, into `grid` (a UAV buffer), then copied to `readback` at `offset`. After Record.
    void RecordGrid(ID3D12GraphicsCommandList*, const ColorConstants& constants, unsigned scale, unsigned words, unsigned rows, ID3D12Resource* grid,
                    ID3D12Resource* readback, std::uint64_t offset) const noexcept;
    // Remote vibrometry's samples (HdrToneMap.hlsl Vibro, 2026-11-03): the region [left, right) x [top, bottom) (luma samples of
    // the coded surface, fractional) as `cells` x `cells` cells, kVibroValues floats each (Y', Y' moved half a sample right,
    // left, down, up, Cb, Cr: 8-bit codes), into `buffer` (a UAV buffer), then copied to `readback` at `offset`. After Record.
    static constexpr unsigned kVibroValues = 7;
    void RecordVibro(ID3D12GraphicsCommandList*, const ColorConstants& constants, float left, float top, float right, float bottom, unsigned cells,
                     ID3D12Resource* buffer, ID3D12Resource* readback, std::uint64_t offset) const noexcept;
    // Tensor export's cells (HdrToneMap.hlsl Tensor, 2026-11-03): the region as `width` x `height` cells, three floats each (Y',
    // Cb, Cr: 8-bit codes), into `buffer`, then copied to `readback` at 0; an HDR picture's source peak (PQ) follows them. After
    // Record or Bind; `peak_in_use`: this list has written the peak state (a conversion's).
    void RecordTensor(ID3D12GraphicsCommandList*, const ColorConstants& constants, float left, float top, float right, float bottom, unsigned width,
                      unsigned height, ID3D12Resource* buffer, ID3D12Resource* readback, bool peak_in_use) const noexcept;
    // Micro-motion's per-pixel reference (one float per output pixel, row of the conversion's width): made or grown here
    // (setup only), kept while large enough. Conversions may set kMicro only for outputs of at most MotionPixels() pixels.
    [[nodiscard]] HRESULT PrepareMotion(ID3D12Device*, std::uint64_t pixels) noexcept;
    [[nodiscard]] std::uint64_t MotionPixels() const noexcept { return motion_pixels_; }
    [[nodiscard]] ID3D12PipelineState* Initial() const noexcept { return sdr_; }
    [[nodiscard]] ID3D12Resource* PeakState() const noexcept { return peak_; } // [0] detector, [4] peak PQ, [8] pictures, [12] cuts, [16] starts.
    void Release() noexcept;
private:
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState *sdr_ = nullptr, *hdr_ = nullptr, *sdr_compare_ = nullptr, *hdr_compare_ = nullptr, *peak_pass_ = nullptr, *detect_pass_ = nullptr, *stats_pass_ = nullptr;
    ID3D12PipelineState* stats_reset_ = nullptr;
    ID3D12PipelineState* grid_pass_ = nullptr;
    ID3D12PipelineState* vibro_pass_ = nullptr;
    ID3D12PipelineState* tensor_pass_ = nullptr;
    // PICTURE_CONTROLS variants (picture_flags != 0), plain and split-comparison.
    ID3D12PipelineState *sdr_picture_ = nullptr, *hdr_picture_ = nullptr, *sdr_compare_picture_ = nullptr, *hdr_compare_picture_ = nullptr;
    ID3D12Resource* peak_ = nullptr;
    ID3D12Resource* blocks_ = nullptr; // The statistics' last sample per 8x8 block (u3).
    ID3D12Resource* motion_ = nullptr; // Micro-motion's reference (u2); peak_ is bound in its place before PrepareMotion.
    std::uint64_t motion_pixels_ = 0;
};
}
