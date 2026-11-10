#include "media/video/FlowRefiner.hpp"
#include <cmath>
#include <cstring>
#if !defined(MEDIA_PERCEPTION_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define MEDIA_REFINER_SSE2 1
#include <emmintrin.h>
#endif

namespace media::video {
namespace {
bool g_scalar = false; // FlowRefiner::ForceScalar: the scalar rows, for the bit-exactness test.
// The tile's bounds as the evaluation draws them (round(i * size / kTiles)), its centre half-way.
float Centre(unsigned index, unsigned size) noexcept {
    const auto bound = [&](unsigned i) { return std::floor(static_cast<double>(i) * size / FlowRefiner::kTiles + 0.5); };
    return static_cast<float>(0.5 * (bound(index) + bound(index + 1)));
}
// One row of the window at unit spacing: the bilinear previous picture's 21 samples against the template row, five running sums
// in four lanes (lane l takes samples l, l + 4, ...; the scalar loop adds in the same order, so the two are bit-identical).
struct Lanes { float sum[4], squares[4], bx[4], by[4], cross[4]; };
inline void RowScalar(const std::uint8_t* p, const std::uint8_t* q, const float* gx, const float* gy, const float* value, int count, float w00, float w10, float w01,
                      float w11, Lanes& l) noexcept {
    for (int i = 0; i < count; ++i) {
        const int lane = i & 3;
        const float v = ((w00 * p[i] + w10 * p[i + 1]) + w01 * q[i]) + w11 * q[i + 1];
        l.sum[lane] += v; l.squares[lane] += v * v; l.bx[lane] += gx[i] * v; l.by[lane] += gy[i] * v; l.cross[lane] += value[i] * v;
    }
}
#if MEDIA_REFINER_SSE2
inline __m128 Four(const std::uint8_t* at) noexcept {
    int bytes; std::memcpy(&bytes, at, 4);
    const __m128i zero = _mm_setzero_si128();
    return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(bytes), zero), zero));
}
inline void RowSse2(const std::uint8_t* p, const std::uint8_t* q, const float* gx, const float* gy, const float* value, int count, float w00, float w10, float w01,
                    float w11, Lanes& l) noexcept {
    const __m128 a = _mm_set1_ps(w00), b = _mm_set1_ps(w10), c = _mm_set1_ps(w01), d = _mm_set1_ps(w11);
    __m128 sum = _mm_loadu_ps(l.sum), squares = _mm_loadu_ps(l.squares), bx = _mm_loadu_ps(l.bx), by = _mm_loadu_ps(l.by), cross = _mm_loadu_ps(l.cross);
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m128 v = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(a, Four(p + i)), _mm_mul_ps(b, Four(p + i + 1))), _mm_mul_ps(c, Four(q + i))), _mm_mul_ps(d, Four(q + i + 1)));
        sum = _mm_add_ps(sum, v); squares = _mm_add_ps(squares, _mm_mul_ps(v, v));
        bx = _mm_add_ps(bx, _mm_mul_ps(_mm_loadu_ps(gx + i), v)); by = _mm_add_ps(by, _mm_mul_ps(_mm_loadu_ps(gy + i), v));
        cross = _mm_add_ps(cross, _mm_mul_ps(_mm_loadu_ps(value + i), v));
    }
    _mm_storeu_ps(l.sum, sum); _mm_storeu_ps(l.squares, squares); _mm_storeu_ps(l.bx, bx); _mm_storeu_ps(l.by, by); _mm_storeu_ps(l.cross, cross);
    // The tail: lanes 0.. of the next group, as the scalar loop would take them.
    for (int k = 0; i < count; ++i, ++k) {
        const float v = ((w00 * p[i] + w10 * p[i + 1]) + w01 * q[i]) + w11 * q[i + 1];
        l.sum[k] += v; l.squares[k] += v * v; l.bx[k] += gx[i] * v; l.by[k] += gy[i] * v; l.cross[k] += value[i] * v;
    }
}
#endif
inline float Total(const float* lanes) noexcept { return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]); }
}

void FlowRefiner::ForceScalar(bool scalar) noexcept { g_scalar = scalar; }

bool FlowRefiner::Configure(unsigned width, unsigned height) noexcept {
    if (width < static_cast<unsigned>(kWindow) + 4 || height < static_cast<unsigned>(kWindow) + 4) return false;
    width_ = width; height_ = height;
    for (unsigned t = 0; t < kTiles * kTiles; ++t) { valid_[t] = refined_[t] = 0; vx_[t] = vy_[t] = 0; }
    return true;
}

float FlowRefiner::CentreX(unsigned tile) const noexcept { return Centre(tile % kTiles, width_); }
float FlowRefiner::CentreY(unsigned tile) const noexcept { return Centre(tile / kTiles, height_); }

unsigned FlowRefiner::Refine(const Perception& perception, const std::uint8_t* previous, unsigned previous_stride, const std::uint8_t* now, unsigned stride,
                             double seconds) noexcept {
    unsigned refined = 0;
    for (unsigned t = 0; t < kTiles * kTiles; ++t) {
        valid_[t] = perception.FlowValid(t) ? 1 : 0; refined_[t] = 0;
        vx_[t] = perception.FlowX(t); vy_[t] = perception.FlowY(t);
        if (!valid_[t] || !previous || !now || !(seconds > 0)) continue;
        float dx = static_cast<float>(vx_[t] * seconds), dy = static_cast<float>(vy_[t] * seconds);
        const int cx = static_cast<int>(std::lround(CentreX(t))), cy = static_cast<int>(std::lround(CentreY(t)));
        // Coarse (samples kCoarse pixels apart: twice the basin) then fine; either may keep the answer it was given.
        const bool coarse = kCoarse > 1 && RefineTile(previous, previous_stride, now, stride, cx, cy, kCoarse, dx, dy);
        const bool fine = RefineTile(previous, previous_stride, now, stride, cx, cy, 1, dx, dy);
        if (!coarse && !fine) continue;
        vx_[t] = static_cast<float>(dx / seconds); vy_[t] = static_cast<float>(dy / seconds);
        refined_[t] = 1; ++refined;
    }
    return refined;
}

bool FlowRefiner::RefineTile(const std::uint8_t* previous, unsigned previous_stride, const std::uint8_t* now, unsigned stride, int cx, int cy, int spacing,
                             float& dx, float& dy) const noexcept {
    constexpr int r = kWindow / 2, n = kWindow * kWindow;
    const int w = static_cast<int>(width_), h = static_cast<int>(height_), s = spacing;
    const std::ptrdiff_t line = static_cast<std::ptrdiff_t>(stride) * s;
    // The template: now's window, kWindow samples `s` pixels apart, one spacing inside the picture for its central differences.
    if (cx - s * (r + 1) < 0 || cy - s * (r + 1) < 0 || cx + s * (r + 1) >= w || cy + s * (r + 1) >= h) return false;
    float gx[n], gy[n], value[n];
    double sx = 0, sy = 0, sv = 0;
    const float half = 0.5f / static_cast<float>(s); // Gradients per pixel.
    for (int j = -r, k = 0; j <= r; ++j) {
        const std::uint8_t* row = now + static_cast<std::ptrdiff_t>(cy + s * j) * static_cast<std::ptrdiff_t>(stride) + cx;
        for (int i = -r; i <= r; ++i, ++k) {
            const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(s) * i;
            value[k] = row[at];
            gx[k] = half * (static_cast<float>(row[at + s]) - static_cast<float>(row[at - s]));
            gy[k] = half * (static_cast<float>(row[at + line]) - static_cast<float>(row[at - line]));
            sx += gx[k]; sy += gy[k]; sv += value[k];
        }
    }
    // Mean-removed gradients and template, their 2x2 system and the template's own sums, once. With both mean-free, the
    // residual's mean drops out of the right-hand side (sum g (s - mean(s)) = sum g s) and its energy is a few running sums:
    // one pass of five products per sample and step, in float (441 terms).
    const float mx = static_cast<float>(sx / n), my = static_cast<float>(sy / n), mv = static_cast<float>(sv / n);
    float hxx = 0, hyy = 0, hxy = 0, tx = 0, ty = 0, tt = 0;
    for (int k = 0; k < n; ++k) {
        gx[k] -= mx; gy[k] -= my; value[k] -= mv;
        hxx += gx[k] * gx[k]; hyy += gy[k] * gy[k]; hxy += gx[k] * gy[k];
        tx += gx[k] * value[k]; ty += gy[k] * value[k]; tt += value[k] * value[k];
    }
    const double det = static_cast<double>(hxx) * hyy - static_cast<double>(hxy) * hxy;
    if (!(det > 1e-4 * static_cast<double>(hxx + hyy) * (hxx + hyy)) || !(hxx + hyy > n * 1.0f / static_cast<float>(s * s))) return false;
    // now(p) = previous(p - d). Inverse compositional on now's window: e = H^-1 sum g (previous(p - d) - now(p)), d += e.
    const float seed_x = dx, seed_y = dy;
    const float cell_x = static_cast<float>(width_) / Perception::kCells, cell_y = static_cast<float>(height_) / Perception::kCells;
    float x = dx, y = dy;
    float seed_cost = -1, last_cost = 0; // The residual's energy where the seed put the window, and at the last position read.
    for (unsigned step = 0; step < kSteps; ++step) {
        const float ox = static_cast<float>(cx - s * r) - x, oy = static_cast<float>(cy - s * r) - y;
        const int ix = static_cast<int>(std::floor(ox)), iy = static_cast<int>(std::floor(oy));
        if (ix < 0 || iy < 0 || ix + s * (kWindow - 1) + 1 >= w || iy + s * (kWindow - 1) + 1 >= h) return false;
        const float fx = ox - static_cast<float>(ix), fy = oy - static_cast<float>(iy);
        const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
        float sum = 0, squares = 0, bx = 0, by = 0, cross = 0;
        if (s == 1) {
            // Each row restarts its lanes at zero so that a row's tail lands in the same lanes in both loops.
            for (int j = 0; j < kWindow; ++j) {
                const std::uint8_t* p = previous + static_cast<std::ptrdiff_t>(iy + j) * static_cast<std::ptrdiff_t>(previous_stride) + ix;
                Lanes l{};
#if MEDIA_REFINER_SSE2
                if (!g_scalar) RowSse2(p, p + previous_stride, gx + j * kWindow, gy + j * kWindow, value + j * kWindow, kWindow, w00, w10, w01, w11, l);
                else
#endif
                RowScalar(p, p + previous_stride, gx + j * kWindow, gy + j * kWindow, value + j * kWindow, kWindow, w00, w10, w01, w11, l);
                sum += Total(l.sum); squares += Total(l.squares); bx += Total(l.bx); by += Total(l.by); cross += Total(l.cross);
            }
        } else {
            for (int j = 0; j < kWindow; ++j) {
                const std::uint8_t* p = previous + static_cast<std::ptrdiff_t>(iy + s * j) * static_cast<std::ptrdiff_t>(previous_stride) + ix;
                const std::uint8_t* q = p + previous_stride;
                const float* gxr = gx + j * kWindow; const float* gyr = gy + j * kWindow; const float* vr = value + j * kWindow;
                for (int i = 0; i < kWindow; ++i) {
                    const int a = s * i;
                    const float v = ((w00 * p[a] + w10 * p[a + 1]) + w01 * q[a]) + w11 * q[a + 1];
                    sum += v; squares += v * v; bx += gxr[i] * v; by += gyr[i] * v; cross += vr[i] * v;
                }
            }
        }
        // sum (v - mean(v) - value)^2, value mean-free.
        const float cost = squares - sum * sum / static_cast<float>(n) - 2 * cross + tt;
        if (seed_cost < 0) seed_cost = cost;
        last_cost = cost;
        const float rx = bx - tx, ry = by - ty; // sum g (v - value)
        // previous(p - d - e) ~ previous(p - d) - g e and g ~ now's gradient: the e that cancels the residual.
        const double ex = (static_cast<double>(hyy) * rx - static_cast<double>(hxy) * ry) / det, ey = (static_cast<double>(hxx) * ry - static_cast<double>(hxy) * rx) / det;
        x += static_cast<float>(ex); y += static_cast<float>(ey);
        if (std::fabs(ex) + std::fabs(ey) < kConverged * static_cast<float>(s)) break;
    }
    if (!(std::fabs(x - seed_x) <= kMaxWalk * cell_x && std::fabs(y - seed_y) <= kMaxWalk * cell_y)) return false;
    // A walk must explain the window better than the seed did (a second minimum it slid into, a window torn by an occlusion).
    if (last_cost > seed_cost) return false;
    dx = x; dy = y;
    return true;
}
}
