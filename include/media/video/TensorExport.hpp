#pragma once
#include "media/video/AreaSample.hpp"
#include <cstddef>
#include <cstdint>

namespace media::video {
// The tensor a model takes (ExportTensor).
struct TensorSpec {
    enum Format : std::uint32_t { kFloat32 = 1, kFloat16 = 2 };
    enum Normalize : std::uint32_t { kUnit = 0, kImageNet = 1, kSigned = 2 };
    unsigned width = 224, height = 224;
    unsigned channels = 3;          // 3: R, G, B planes (B, G, R with `bgr`); 1: luma (Y', 0..1).
    Format format = kFloat32;
    Normalize normalize = kUnit;    // kUnit [0, 1]; kImageNet (v - mean) / std per channel; kSigned [-1, 1].
    bool bgr = false;
};
// ImageNet's statistics (R, G, B), and the usual pair for one grey channel.
inline constexpr float kImageNetMean[3]{0.485f, 0.456f, 0.406f}, kImageNetStd[3]{0.229f, 0.224f, 0.225f};
inline constexpr float kGrayMean = 0.449f, kGrayStd = 0.226f;

// The bytes of a spec's tensor (1 x channels x height x width elements); 0 for an invalid spec.
[[nodiscard]] std::size_t TensorBytes(const TensorSpec& spec) noexcept;
// IEEE 754 binary16 from a float, rounded to nearest even (subnormals, infinities and NaN kept).
[[nodiscard]] std::uint16_t HalfFromFloat(float value) noexcept;

// What a picture's codes mean (2026-11-04; docs/picture.md, "HDR tensors"). Transfer 16 (PQ, SMPTE ST 2084) and 18 (HLG): an
// element is not its coded values but what the screen shows for them, in [0, 1]: BT.2020 Y'CbCr to R'G'B', the transfer's
// display light, the BT.2390 tone map from `peak_nits` to SDR white (203 cd/m2) on the brightest channel, BT.2020 to BT.709
// primaries, gamma 2.4 (HdrToneMap.hlsl Convert, the same arithmetic). Any other transfer: BT.709 limited range, as coded.
struct TensorColor {
    unsigned transfer = 0;    // H.273's: 0 unspecified.
    bool full_range = false;  // The codes' range (HDR only).
    bool ten_bit = false;     // The codes are a 10-bit source's in 8-bit units (the code / 4).
    float peak_nits = 1000;   // The source's peak: the windowed engine's detected peak, as the picture on screen is converted.
};
[[nodiscard]] inline constexpr bool IsHdrTensor(const TensorColor& c) noexcept { return c.transfer == 16 || c.transfer == 18; }
[[nodiscard]] double PqToNits(double pq) noexcept; // SMPTE ST 2084, [0, 1] -> cd/m2.
[[nodiscard]] double NitsToPq(double nits) noexcept;
// One Y'CbCr sample (8-bit units) to display-encoded BT.709 R'G'B' in [0, 1] (above).
void HdrToDisplay(const TensorColor& color, double y, double cb, double cr, double rgb[3]) noexcept;

// A region of a picture as a model's input tensor (docs/picture.md, "Tensor export", 2026-11-02), portable, no allocation:
// NCHW (N = 1), each channel a contiguous height x width plane, rows top to bottom, in the caller's buffer. Each element is
// the region's cell (the region divided into width x height cells) area-averaged (AreaMean: a box filter when shrinking, the
// samples' squares when enlarging) on each plane, then converted: BT.709 limited range (Y' (Y - 16) / 219, Cb and Cr about
// 128 over 224) to R'G'B' clamped to [0, 1] (gamma-encoded, as images are given to models), and normalised. In double; each
// element rounded once to its format. False for an invalid spec, a region off the picture or too small a buffer.
// `color`: an HDR picture's elements as the screen shows them (TensorColor; a Y' tensor their BT.709 luma, R'G'B' weighted).
bool ExportTensor(const Planes420& picture, AreaRect region, const TensorSpec& spec, void* out, std::size_t bytes, const TensorColor& color = {}) noexcept;
// The same from the cells' means already taken (2026-11-03: the windowed engine's GPU pass, HdrToneMap.hlsl Tensor): `means`
// holds width x height elements of three floats, Y', Cb and Cr in 8-bit codes, row-major; converted and normalised as above.
bool ExportTensorMeans(const float* means, const TensorSpec& spec, void* out, std::size_t bytes, const TensorColor& color = {}) noexcept;
}
