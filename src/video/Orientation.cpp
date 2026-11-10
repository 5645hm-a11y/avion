#include "media/video/Orientation.hpp"
#include <algorithm>
#include <cmath>

namespace media::video {
bool Orientation::Axes(const std::int8_t t[4]) noexcept {
    if (!t) return false;
    for (unsigned i = 0; i < 4; ++i) if (t[i] < -1 || t[i] > 1) return false;
    return (t[0] && t[3] && !t[1] && !t[2]) || (!t[0] && !t[3] && t[1] && t[2]);
}
void Orientation::Display(int d[4]) const noexcept {
    // The file's matrix as rows (a c; b d), or the identity when it is not of the axes; then each quarter turn clockwise,
    // (x, y) -> (-y, x): the rows become (-row 1, row 0).
    const bool axes = Axes(file);
    int m[4]{axes ? file[0] : 1, axes ? file[2] : 0, axes ? file[1] : 0, axes ? file[3] : 1};
    for (unsigned k = 0; k < turns % 4; ++k) { const int r[4]{-m[2], -m[3], m[0], m[1]}; for (unsigned i = 0; i < 4; ++i) m[i] = r[i]; }
    for (unsigned i = 0; i < 4; ++i) d[i] = m[i];
}
bool Orientation::Mirrored() const noexcept { int d[4]; Display(d); return d[0] * d[3] - d[1] * d[2] < 0; }
bool Orientation::Across() const noexcept { int d[4]; Display(d); return d[0] == 0; }
bool Orientation::Upright() const noexcept { int d[4]; Display(d); return d[0] == 1 && d[3] == 1 && level == 0; }
unsigned Orientation::QuarterTurns() const noexcept {
    int d[4]; Display(d);
    if (d[0] * d[3] - d[1] * d[2] < 0) { d[0] = -d[0]; d[2] = -d[2]; } // A mirror (x -> -x) taken out first.
    return d[0] == 1 ? 0u : d[2] == 1 ? 1u : d[0] == -1 ? 2u : 3u;   // Where the x axis goes: right, down, left, up.
}
void Orientation::Map(double w, double h, double rw, double rh, double m[4], double t[2]) const noexcept {
    int d[4]; Display(d);
    const bool across = d[0] == 0;
    // Output centre offset e -> the leveling turn undone and zoomed in by f, R(-level) e / f -> samples of the region as shown
    // (diag(sx, sy)) -> the region's own axes (D's inverse, its transpose), about the region's centre.
    const double shown_w = across ? rh : rw, shown_h = across ? rw : rh;
    const double radians = static_cast<double>(level) * 3.14159265358979323846 / 180.0, co = std::cos(radians), si = std::sin(radians);
    const double f = std::fabs(co) + std::fabs(si) * (std::max)(w / h, h / w);
    const double sx = shown_w / w, sy = shown_h / h;
    const double s[4]{sx * co / f, sx * si / f, -sy * si / f, sy * co / f};
    m[0] = d[0] * s[0] + d[2] * s[2]; m[1] = d[0] * s[1] + d[2] * s[3];
    m[2] = d[1] * s[0] + d[3] * s[2]; m[3] = d[1] * s[1] + d[3] * s[3];
    t[0] = rw / 2 - (m[0] * w / 2 + m[1] * h / 2);
    t[1] = rh / 2 - (m[2] * w / 2 + m[3] * h / 2);
}
void Orientation::ToStored(float& x, float& y) const noexcept {
    float u = x - 0.5f, v = y - 0.5f; DirectionToStored(u, v); x = 0.5f + u; y = 0.5f + v;
}
void Orientation::DirectionToStored(float& dx, float& dy) const noexcept {
    int d[4]; Display(d);
    const float u = dx, v = dy;
    dx = static_cast<float>(d[0]) * u + static_cast<float>(d[2]) * v; dy = static_cast<float>(d[1]) * u + static_cast<float>(d[3]) * v;
}
}
