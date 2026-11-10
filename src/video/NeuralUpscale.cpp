#include "media/video/NeuralUpscale.hpp"
#include <algorithm>

namespace media::video {
namespace {
float At(const LumaView& v, int x, int y) noexcept {
    x = std::clamp(x, 0, static_cast<int>(v.width) - 1); y = std::clamp(y, 0, static_cast<int>(v.height) - 1);
    return v.data[static_cast<std::size_t>(y) * v.stride + static_cast<std::size_t>(x)];
}
// Bilinear value at sub-pixel s of source pixel (x, y): a quarter pixel towards the neighbours in -/+ x and y.
float SubPixelBase(const LumaView& v, int x, int y, int sx, int sy) noexcept {
    const int nx = sx ? x + 1 : x - 1, ny = sy ? y + 1 : y - 1;
    return 0.5625f * At(v, x, y) + 0.1875f * At(v, nx, y) + 0.1875f * At(v, x, ny) + 0.0625f * At(v, nx, ny);
}
void Features1(const LumaView& v, const NeuralModel& m, int x, int y, float (&f1)[NeuralModel::kFeatures1]) noexcept {
    float taps[9];
    for (int k = 0; k < 9; ++k) taps[k] = At(v, x + k % 3 - 1, y + k / 3 - 1);
    for (unsigned c = 0; c < NeuralModel::kFeatures1; ++c) {
        float sum = m.conv1_bias[c];
        for (int k = 0; k < 9; ++k) sum += m.conv1_weight[c][k] * taps[k];
        f1[c] = LeakyRelu(sum);
    }
}
}
// Flat-tile rule: the clamped 20x20 neighbourhood of the tile containing (x, y) spans less than kFlatRange.
static bool FlatTile(const LumaView& v, int x, int y) noexcept {
    const int tx = x / static_cast<int>(kTile) * static_cast<int>(kTile), ty = y / static_cast<int>(kTile) * static_cast<int>(kTile);
    float lo = At(v, tx - 2, ty - 2), hi = lo;
    for (int j = ty - 2; j < ty + static_cast<int>(kTile) + 2; ++j)
        for (int i = tx - 2; i < tx + static_cast<int>(kTile) + 2; ++i) { const float s = At(v, i, j); lo = std::min(lo, s); hi = std::max(hi, s); }
    return hi - lo < kFlatRange;
}
void NeuralUpscaleReference(const LumaView& v, const NeuralModel& m, float* out, std::size_t out_stride) noexcept {
    if (!v.data || !v.width || !v.height || !out) return;
    for (int y = 0; y < static_cast<int>(v.height); ++y)
        for (int x = 0; x < static_cast<int>(v.width); ++x) {
            const bool flat = FlatTile(v, x, y); // Residual 0 (the GPU skips the network for the whole tile).
            float f2[NeuralModel::kFeatures2]{};
            if (!flat) {
                float f1[9][NeuralModel::kFeatures1];
                for (int k = 0; k < 9; ++k) Features1(v, m, x + k % 3 - 1, y + k / 3 - 1, f1[k]);
                for (unsigned o = 0; o < NeuralModel::kFeatures2; ++o) {
                    float sum = m.conv2_bias[o];
                    for (unsigned c = 0; c < NeuralModel::kFeatures1; ++c)
                        for (int k = 0; k < 9; ++k) sum += m.conv2_weight[o][c][k] * f1[k][c];
                    f2[o] = LeakyRelu(sum);
                }
            }
            for (int s = 0; s < 4; ++s) {
                float residual = flat ? 0.0f : m.conv3_bias[s];
                for (unsigned c = 0; c < NeuralModel::kFeatures2; ++c) residual += m.conv3_weight[s][c] * f2[c];
                const int sx = s & 1, sy = s >> 1;
                // Deringing: within the range of the four samples the bilinear base interpolates.
                const int nx = sx ? x + 1 : x - 1, ny = sy ? y + 1 : y - 1;
                const float q[4]{At(v, x, y), At(v, nx, y), At(v, x, ny), At(v, nx, ny)};
                const float lo = std::min(std::min(q[0], q[1]), std::min(q[2], q[3])), hi = std::max(std::max(q[0], q[1]), std::max(q[2], q[3]));
                const float value = std::clamp(SubPixelBase(v, x, y, sx, sy) + residual, lo, hi);
                out[static_cast<std::size_t>(2 * y + sy) * out_stride + static_cast<std::size_t>(2 * x + sx)] = value;
            }
        }
}
void BilinearUpscaleReference(const LumaView& v, float* out, std::size_t out_stride) noexcept {
    if (!v.data || !v.width || !v.height || !out) return;
    for (int y = 0; y < static_cast<int>(v.height); ++y)
        for (int x = 0; x < static_cast<int>(v.width); ++x)
            for (int s = 0; s < 4; ++s) {
                // Clamped like the network's output (a no-op mathematically; it removes float rounding past the range).
                const int sx = s & 1, sy = s >> 1, nx = sx ? x + 1 : x - 1, ny = sy ? y + 1 : y - 1;
                const float q[4]{At(v, x, y), At(v, nx, y), At(v, x, ny), At(v, nx, ny)};
                out[static_cast<std::size_t>(2 * y + sy) * out_stride + static_cast<std::size_t>(2 * x + sx)] =
                    std::clamp(SubPixelBase(v, x, y, sx, sy), std::min(std::min(q[0], q[1]), std::min(q[2], q[3])), std::max(std::max(q[0], q[1]), std::max(q[2], q[3])));
            }
}
}
