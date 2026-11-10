#pragma once
#include <cstddef>
#include <cstdint>

namespace media::video {
// Micro-ESPCN luma super-resolution (2x), residual over bilinear. Every layer is evaluated at integer source
// positions p with the input sampled at clamp(p) to the source rectangle (layers are *not* clamped individually):
//   f1(p) = lrelu(b1 + sum_k W1[k] * Y(clamp(p + d_k)))                 16 maps, 3x3
//   f2(p) = lrelu(b2 + sum_c sum_k W2[c][k] * f1(p + d_k)[c])            8 maps, 3x3 (over 16 maps)
//   r(p)  = b3 + W3 * f2(p)                                              4 sub-pixel residuals, 1x1
//   out(2p + s) = clamp(bilinear(Y, p + (s - 0.5) / 2) + r(p)[s], lo, hi) s = (sx, sy) in {0,1}^2, r index sy*2 + sx
//   where [lo, hi] is the range of the four samples that bilinear term interpolates (deringing: no halo can form;
//   on held-out photos this also raised PSNR by 0.14 dB, since the unclamped overshoot was error).
// with d_k = (k % 3 - 1, k / 3 - 1) and lrelu(x) = max(x, 0.1 x). Bilinear sub-pixel weights are 9/16, 3/16, 3/16,
// 1/16 (offsets of a quarter source pixel). Y is the encoded luma as sampled (UNORM, [0, 1]).
// Flat tiles: the source is split into 16x16 tiles from its origin; when the (clamped) 20x20 neighbourhood of a tile,
// [16 tx - 2, 16 tx + 18)^2, spans less than kFlatRange, r = 0 for every pixel of the tile (the GPU skips the network
// there). On exactly flat input the network's residual is below 0.08 of an 8-bit code, so this changes nothing visible.
struct NeuralModel {
    static constexpr unsigned kFeatures1 = 16, kFeatures2 = 8, kOutputs = 4, kTaps = 9;
    float conv1_weight[kFeatures1][kTaps];
    float conv1_bias[kFeatures1];
    float conv2_weight[kFeatures2][kFeatures1][kTaps];
    float conv2_bias[kFeatures2];
    float conv3_weight[kOutputs][kFeatures2];
    float conv3_bias[kOutputs];
};
static_assert(sizeof(NeuralModel) == 1356 * sizeof(float), "Packed as the shader's constant buffer (1356 floats).");
constexpr float kLeakySlope = 0.1f;
constexpr float kFlatRange = 1.5f / 255.0f;
constexpr unsigned kTile = 16;
inline float LeakyRelu(float x) noexcept { return x > 0 ? x : kLeakySlope * x; }

// Source image view: `width` x `height` samples, rows `stride` floats apart.
struct LumaView { const float* data = nullptr; unsigned width = 0, height = 0; std::size_t stride = 0; };
// CPU reference of the shader: writes the 2x image (2 width x 2 height, row stride `out_stride`). Allocation-free
// (features are recomputed per pixel); intended for tests and training checks, not real time.
void NeuralUpscaleReference(const LumaView& source, const NeuralModel& model, float* out, std::size_t out_stride) noexcept;
// Bilinear 2x (the residual's base) alone, for comparisons.
void BilinearUpscaleReference(const LumaView& source, float* out, std::size_t out_stride) noexcept;
}
