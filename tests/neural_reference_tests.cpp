// Portable checks of the neural upscaler's definition (media::video::NeuralUpscaleReference) and of the committed
// weights (MSVC, GCC -Werror, ASan/UBSan). The GPU pass is checked against this reference in media_neural_upscale_tests.
#include "media/video/NeuralUpscale.hpp"
#include "media/render/NeuralWeights.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using media::video::NeuralModel;
using media::video::LumaView;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
std::uint32_t seed = 99;
float Random() { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; }
float At(const std::vector<float>& image, int w, int h, int x, int y) { return image[static_cast<std::size_t>(std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1))]; }
// Every output within the range of the four samples its bilinear base interpolates, and finite.
unsigned OutOfRange(const std::vector<float>& image, int w, int h, const std::vector<float>& out) {
    unsigned bad = 0;
    for (int y = 0; y < 2 * h; ++y) for (int x = 0; x < 2 * w; ++x) {
        const int px = x / 2, py = y / 2, nx = x % 2 ? px + 1 : px - 1, ny = y % 2 ? py + 1 : py - 1;
        const float q[4]{At(image, w, h, px, py), At(image, w, h, nx, py), At(image, w, h, px, ny), At(image, w, h, nx, ny)};
        const float v = out[static_cast<std::size_t>(y * 2 * w + x)];
        bad += !std::isfinite(v) || v < *std::min_element(q, q + 4) || v > *std::max_element(q, q + 4);
    }
    return bad;
}
}
int main() {
    static_assert(sizeof(NeuralModel) < 64 * 1024, "Model footprint must stay under 64 KB.");
    constexpr int W = 45, H = 37; // Not tile multiples.
    std::vector<float> image(W * H), out(4 * W * H), bilinear(4 * W * H), again(4 * W * H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
        image[static_cast<std::size_t>(y * W + x)] = std::clamp(((x + y) > 40 ? 0.8f : 0.2f) + (Random() - 0.5f) * 0.05f, 0.0f, 1.0f);
    const LumaView view{image.data(), W, H, W};
    // Zero weights: exactly the bilinear base (no residual), which never leaves its sample range.
    const NeuralModel zero{};
    media::video::NeuralUpscaleReference(view, zero, out.data(), 2 * W);
    media::video::BilinearUpscaleReference(view, bilinear.data(), 2 * W);
    CHECK(out == bilinear);
    // Committed weights: in range, deterministic, allocation-free, and different from bilinear (it does something).
    allocation_probe::Begin();
    media::video::NeuralUpscaleReference(view, media::render::kNeuralWeights, out.data(), 2 * W);
    media::video::NeuralUpscaleReference(view, media::render::kNeuralWeights, again.data(), 2 * W);
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0 && out == again && out != bilinear && OutOfRange(image, W, H, out) == 0);
    // Adversarial weights (random, x50) on extreme inputs: still finite and inside the deringing range.
    NeuralModel wild{}; auto* w = reinterpret_cast<float*>(&wild);
    for (unsigned i = 0; i < sizeof(wild) / sizeof(float); ++i) w[i] = (Random() * 2 - 1) * 50;
    for (int pattern = 0; pattern < 3; ++pattern) {
        std::vector<float> extreme(W * H);
        for (int i = 0; i < W * H; ++i) extreme[static_cast<std::size_t>(i)] = pattern == 0 ? static_cast<float>((i % W + i / W) % 2) : pattern == 1 ? Random() : (i % 7 == 0 ? 1.0f : 0.0f);
        media::video::NeuralUpscaleReference({extreme.data(), W, H, W}, wild, out.data(), 2 * W);
        CHECK(OutOfRange(extreme, W, H, out) == 0);
        media::video::NeuralUpscaleReference({extreme.data(), W, H, W}, media::render::kNeuralWeights, out.data(), 2 * W);
        CHECK(OutOfRange(extreme, W, H, out) == 0);
    }
    // Flat tiles: a 1-code (8-bit) dither is below kFlatRange, so the output is exactly the bilinear base; a 2-code step
    // is not flat. Tile boundaries: flatness is decided per 16x16 tile with a 2-pixel ring.
    {
        std::vector<float> flat(W * H);
        for (int i = 0; i < W * H; ++i) flat[static_cast<std::size_t>(i)] = (100 + (i % 3 == 0)) / 255.0f;
        media::video::NeuralUpscaleReference({flat.data(), W, H, W}, wild, out.data(), 2 * W);
        media::video::BilinearUpscaleReference({flat.data(), W, H, W}, bilinear.data(), 2 * W);
        CHECK(out == bilinear);
        // Detail only in the last columns: tiles x < 16 (ring reaching column 17) stay flat, the tile holding it does not.
        for (int y = 0; y < H; ++y) flat[static_cast<std::size_t>(y * W + 40)] = 0.9f;
        media::video::NeuralUpscaleReference({flat.data(), W, H, W}, wild, out.data(), 2 * W);
        media::video::BilinearUpscaleReference({flat.data(), W, H, W}, bilinear.data(), 2 * W);
        bool first_tiles_bilinear = true, detail_tile_differs = false;
        for (int y = 0; y < 2 * H; ++y) for (int x = 0; x < 2 * W; ++x) {
            const bool same = out[static_cast<std::size_t>(y * 2 * W + x)] == bilinear[static_cast<std::size_t>(y * 2 * W + x)];
            if (x < 32) first_tiles_bilinear = first_tiles_bilinear && same;
            if (x >= 64) detail_tile_differs = detail_tile_differs || !same;
        }
        CHECK(first_tiles_bilinear && detail_tile_differs);
    }
    // Committed weights on flat input at every 8-bit level: exactly flat output.
    {
        double worst = 0; std::vector<float> flat(64), up(256);
        for (int code = 0; code <= 255; ++code) {
            std::fill(flat.begin(), flat.end(), static_cast<float>(code) / 255.0f);
            media::video::NeuralUpscaleReference({flat.data(), 8, 8, 8}, media::render::kNeuralWeights, up.data(), 16);
            for (float v : up) worst = std::max(worst, std::fabs(static_cast<double>(v) - code / 255.0));
        }
        CHECK(worst < 1e-6);
    }
    std::printf("Neural reference: %d failures, %llu allocations, model %zu bytes\n", failures, static_cast<unsigned long long>(allocations), sizeof(NeuralModel));
    return failures ? 1 : 0;
}
