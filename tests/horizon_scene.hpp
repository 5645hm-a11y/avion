#pragma once
// The auto-leveling tests' scene (docs/picture.md, "Auto-leveling the horizon"): sky over a horizon at 0.45 of the height,
// three buildings standing on it (dark blocks with lit windows: horizontal and vertical edges), a textured ground (edges every
// way), drawn turned clockwise by the tilt about the picture's centre as a camera sees it: 4 x 4 samples a pixel (the sensor's
// area), then a [1 2 1] / 4 blur each way (the lens, about 0.7 px). Limited-range 8-bit luma. Used by media_auto_horizon_tests,
// the --horizon playback fixture and the demo's check of it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace horizon_scene {
inline std::uint32_t Hash(std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
inline double Noise(double u, double v, std::uint32_t seed) {
    const double fu = std::floor(u), fv = std::floor(v), a = u - fu, b = v - fv;
    const auto at = [&](int i, int j) { return static_cast<double>(Hash(static_cast<std::uint32_t>(i) * 7919u + static_cast<std::uint32_t>(j) * 104729u + seed) % 181u); };
    const int i = static_cast<int>(fu), j = static_cast<int>(fv);
    return (1 - a) * (1 - b) * at(i, j) + a * (1 - b) * at(i + 1, j) + (1 - a) * b * at(i, j + 1) + a * b * at(i + 1, j + 1);
}
// The upright scene at (u, v) of a w x h picture.
inline double Upright(double u, double v, double w, double h) {
    if (v >= 0.45 * h) return 70 + 0.45 * Noise(u / 7, v / 7, 9); // The ground.
    struct Building { double x0, x1, top; };
    static constexpr Building kBuildings[]{{0.12, 0.24, 0.22}, {0.55, 0.63, 0.30}, {0.78, 0.92, 0.26}};
    for (const auto& b : kBuildings)
        if (u >= b.x0 * w && u < b.x1 * w && v >= b.top * h) {
            const bool lit = static_cast<long>(std::floor((u - b.x0 * w) / (0.02 * w))) % 2 == 1 && static_cast<long>(std::floor((v - b.top * h) / (0.03 * h))) % 2 == 1;
            return lit ? 150 : 45;
        }
    return 185 + 15 * (Noise(u / 120, v / 60, 5) / 180 - 0.5); // Soft clouds.
}
// Output p shows the upright point R(-tilt) (p - c) + c: the scene turned clockwise by `tilt` degrees.
inline void Draw(double tilt, unsigned w, unsigned h, std::vector<std::uint8_t>& luma) {
    constexpr double kPi = 3.14159265358979323846;
    std::vector<double> area(static_cast<std::size_t>(w) * h);
    const double r = tilt * kPi / 180, co = std::cos(r), si = std::sin(r), cx = w / 2.0, cy = h / 2.0;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            double sum = 0;
            for (int s = 0; s < 16; ++s) {
                const double px = x + 0.125 + 0.25 * (s & 3) - cx, py = y + 0.125 + 0.25 * (s >> 2) - cy;
                sum += Upright(co * px + si * py + cx, -si * px + co * py + cy, w, h);
            }
            area[static_cast<std::size_t>(y) * w + x] = sum / 16;
        }
    luma.assign(static_cast<std::size_t>(w) * h, 0);
    const auto at = [&](int x, int y) { return area[static_cast<std::size_t>(std::clamp(y, 0, static_cast<int>(h) - 1)) * w + static_cast<unsigned>(std::clamp(x, 0, static_cast<int>(w) - 1))]; };
    for (int y = 0; y < static_cast<int>(h); ++y)
        for (int x = 0; x < static_cast<int>(w); ++x) {
            double sum = 0;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) sum += (j ? 1 : 2) * (i ? 1 : 2) * at(x + i, y + j);
            luma[static_cast<std::size_t>(y) * w + static_cast<unsigned>(x)] = static_cast<std::uint8_t>(std::clamp(std::lround(sum / 16), 0l, 255l));
        }
}
// Black bars encoded in the picture (docs/picture.md, "Black bars"): `rows` of limited-range black (16) at the top and the bottom
// (a letterbox), `columns` at the left and the right (a pillarbox), with an encoder's +-1 code of noise when `seed` is not 0.
inline void Bars(std::vector<std::uint8_t>& luma, unsigned w, unsigned h, unsigned rows, unsigned columns, std::uint32_t seed = 0) {
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            if (y < rows || y >= h - rows || x < columns || x >= w - columns)
                luma[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(seed ? 15 + Hash(y * 7919u + x * 31u + seed) % 3u : 16u);
}
}
