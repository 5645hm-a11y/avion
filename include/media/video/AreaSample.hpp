#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace media::video {
// One picture's 8-bit 4:2:0 planes (the visible picture: `width` x `height` luma samples).
struct Planes420 {
    const std::uint8_t* y = nullptr; const std::uint8_t* cb = nullptr; const std::uint8_t* cr = nullptr;
    unsigned luma_stride = 0, chroma_stride = 0, width = 0, height = 0;
};

// A rectangle of a plane in samples, with fractional edges (right, bottom exclusive).
struct AreaRect { double left = 0, top = 0, right = 0, bottom = 0; };

// The mean of an 8-bit plane over a rectangle with fractional edges (2026-11-02; Vibrometry, TensorExport): the plane is read
// as constant over each sample's unit square, so a rectangle moved by a fraction of a sample changes the mean continuously
// (sub-sample placement without interpolating the samples, and a box filter when the rectangle spans many). The rectangle is
// clipped to the plane; -1 when nothing of it lies on the plane. Exact sums of the whole samples inside (integers), the edge
// rows and columns weighted by their coverage.
inline double AreaMean(const std::uint8_t* plane, unsigned stride, unsigned width, unsigned height, AreaRect r) noexcept {
    const double x0 = r.left > 0 ? r.left : 0, y0 = r.top > 0 ? r.top : 0;
    const double x1 = r.right < width ? r.right : width, y1 = r.bottom < height ? r.bottom : height;
    if (!(x1 > x0) || !(y1 > y0)) return -1;
    const auto c0 = static_cast<unsigned>(std::floor(x0)), r0 = static_cast<unsigned>(std::floor(y0));
    const auto c1 = static_cast<unsigned>(std::ceil(x1)), r1 = static_cast<unsigned>(std::ceil(y1));
    // The first and last columns' coverage (one column: the rectangle's width).
    const double first = c1 - c0 == 1 ? x1 - x0 : (c0 + 1) - x0, last = c1 - c0 == 1 ? 0 : x1 - (c1 - 1);
    double sum = 0;
    for (unsigned y = r0; y < r1; ++y) {
        const double wy = (y + 1 < y1 ? y + 1 : y1) - (y > y0 ? y : y0);
        const std::uint8_t* row = plane + static_cast<std::size_t>(y) * stride;
        std::uint32_t inner = 0;
        for (unsigned x = c0 + 1; x + 1 < c1; ++x) inner += row[x];
        double s = first * row[c0] + inner;
        if (c1 - c0 > 1) s += last * row[c1 - 1];
        sum += wy * s;
    }
    return sum / ((x1 - x0) * (y1 - y0));
}
}
