#include "media/video/VirtualImu.hpp"
#include <algorithm>
#include <cmath>

namespace media::video {
namespace {
constexpr float kCode = 1.0f / 16; // A grid's unit in codes.
// The grid at (x, y) in cells (cell centres at whole numbers), bilinear, in codes; false outside it.
bool Sample(const std::uint16_t* g, unsigned w, unsigned h, float x, float y, float& out) noexcept {
    if (!(x >= 0) || !(y >= 0) || x > static_cast<float>(w - 1) || y > static_cast<float>(h - 1)) return false;
    const unsigned x0 = (std::min)(static_cast<unsigned>(x), w - 2), y0 = (std::min)(static_cast<unsigned>(y), h - 2);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    const std::uint16_t* r0 = g + static_cast<std::size_t>(y0) * w + x0; const std::uint16_t* r1 = r0 + w;
    const float a = static_cast<float>(r0[0]), b = static_cast<float>(r0[1]), c = static_cast<float>(r1[0]), d = static_cast<float>(r1[1]);
    out = ((a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy) * kCode;
    return true;
}
float Median(float* v, unsigned n) noexcept {
    std::nth_element(v, v + n / 2, v + n);
    return v[n / 2];
}
}

bool VirtualImu::Configure(core::LinearMemoryArena& arena, unsigned width, unsigned height, float fov) noexcept {
    grid_[0] = grid_[1] = half_[0] = half_[1] = nullptr;
    if (width < 32 || height < 32 || width > 16384 || height > 16384) return false;
    scale_ = (std::max)({1u, (width + kMaxWidth - 1) / kMaxWidth, (height + kMaxHeight - 1) / kMaxHeight});
    columns_ = width / scale_; rows_ = height / scale_; width_ = width; height_ = height;
    const std::size_t cells = std::size_t{columns_} * rows_, half = std::size_t{columns_ / 2} * (rows_ / 2);
    for (unsigned i = 0; i < 2; ++i) {
        grid_[i] = static_cast<std::uint16_t*>(arena.Allocate(sizeof(std::uint16_t) * cells, 64));
        half_[i] = static_cast<std::uint16_t*>(arena.Allocate(sizeof(std::uint16_t) * half, 64));
        if (!grid_[i] || !half_[i]) { grid_[0] = grid_[1] = half_[0] = half_[1] = nullptr; return false; }
    }
    const double half_fov = (fov > 0 ? static_cast<double>(fov) : 1.5707963267948966) * 0.5;
    focal_ = 0.5 * width / std::tan(half_fov);
    pictures_ = measured_ = 0;
    Reset();
    return true;
}
void VirtualImu::Reset() noexcept { previous_ = false; last_x_ = last_y_ = 0; result_ = {}; }
void VirtualImu::Decimate(const std::uint8_t* luma, unsigned stride, std::uint16_t* out) const noexcept {
    // Box means; a block of 4 x 4 samples or more is read every second sample each way (its mean barely changes).
    const unsigned step = scale_ >= 4 ? 2 : 1, taken = (scale_ + step - 1) / step, area = taken * taken;
    for (unsigned j = 0; j < rows_; ++j) {
        std::uint16_t* row = out + static_cast<std::size_t>(j) * columns_;
        std::uint32_t sums[kMaxWidth]{}; // Rows of source samples in order: the block's first line, then its next, as they lie in memory.
        for (unsigned sy = 0; sy < scale_; sy += step) {
            const std::uint8_t* src = luma + static_cast<std::size_t>(j * scale_ + sy) * stride;
            for (unsigned i = 0; i < columns_; ++i) {
                unsigned sum = 0;
                for (unsigned sx = 0; sx < scale_; sx += step) sum += src[i * scale_ + sx];
                sums[i] += sum;
            }
        }
        for (unsigned i = 0; i < columns_; ++i) row[i] = static_cast<std::uint16_t>((sums[i] * 16 + area / 2) / area);
    }
}
void VirtualImu::Halve(const std::uint16_t* g, std::uint16_t* out) const noexcept {
    const unsigned w = columns_ / 2, h = rows_ / 2;
    for (unsigned j = 0; j < h; ++j)
        for (unsigned i = 0; i < w; ++i) {
            const std::uint16_t* a = g + static_cast<std::size_t>(2 * j) * columns_ + 2 * i;
            out[static_cast<std::size_t>(j) * w + i] = static_cast<std::uint16_t>((a[0] + a[1] + a[columns_] + a[columns_ + 1] + 2u) / 4);
        }
}
const VirtualImuResult& VirtualImu::Push(const std::uint8_t* luma, unsigned stride, const PerceivedObject* objects, unsigned object_count) noexcept {
    result_ = {};
    if (!Configured() || !luma) return result_;
    ++pictures_;
    std::uint16_t* now = grid_[current_];
    Decimate(luma, stride, now); Halve(now, half_[current_]);
    if (!previous_) { previous_ = true; current_ ^= 1u; return result_; }
    const std::uint16_t* then = grid_[current_ ^ 1u];
    // The global motion: the half grids' mean-removed SAD (every second cell each way) about the last motion.
    const unsigned hw = columns_ / 2, hh = rows_ / 2;
    const std::uint16_t* hn = half_[current_]; const std::uint16_t* ht = half_[current_ ^ 1u];
    const int cx = static_cast<int>(std::lround(last_x_ / 2)), cy = static_cast<int>(std::lround(last_y_ / 2));
    const int margin = kSearch + (std::max)(std::abs(cx), std::abs(cy)) + 1;
    double best = INFINITY; int bx = cx, by = cy;
    if (static_cast<int>(hw) > 2 * margin + 8 && static_cast<int>(hh) > 2 * margin + 8)
        for (int sy = cy - kSearch; sy <= cy + kSearch; ++sy)
            for (int sx = cx - kSearch; sx <= cx + kSearch; ++sx) {
                // Content moved by s: now(x) = then(x - s).
                double a = 0, b = 0; unsigned n = 0;
                for (int y = margin; y < static_cast<int>(hh) - margin; y += 2)
                    for (int x = margin; x < static_cast<int>(hw) - margin; x += 2) { a += hn[y * hw + x]; b += ht[(y - sy) * hw + (x - sx)]; ++n; }
                const auto offset = n ? static_cast<int>(std::lround((a - b) / n)) : 0;
                std::int64_t cost = 0;
                for (int y = margin; y < static_cast<int>(hh) - margin; y += 2)
                    for (int x = margin; x < static_cast<int>(hw) - margin; x += 2) cost += std::abs(static_cast<int>(hn[y * hw + x]) - static_cast<int>(ht[(y - sy) * hw + (x - sx)]) - offset);
                const auto sad = static_cast<double>(cost);
                // The nearer of two equal costs (a flat picture keeps the last motion).
                if (sad < best || (sad == best && std::abs(sx - cx) + std::abs(sy - cy) < std::abs(bx - cx) + std::abs(by - cy))) { best = sad; bx = sx; by = sy; }
            }
    const float gx = 2.0f * static_cast<float>(bx), gy = 2.0f * static_cast<float>(by);
    // Each tile from there: Lucas-Kanade on the bilinear previous grid, the tile's own (centred) gradients.
    constexpr unsigned kTiles = kTilesX * kTilesY;
    float move_x[kTiles], move_y[kTiles], at_x[kTiles], at_y[kTiles]; bool on_object[kTiles]; unsigned tiles = 0, free = 0;
    const unsigned tw = columns_ / kTilesX, th = rows_ / kTilesY;
    const float ox = 0.5f * static_cast<float>(columns_ - 1), oy = 0.5f * static_cast<float>(rows_ - 1);
    for (unsigned t = 0; t < kTiles && tw >= 6 && th >= 6; ++t) {
        const unsigned x0 = (std::max)(2u, (t % kTilesX) * tw), x1 = (std::min)(columns_ - 2, (t % kTilesX + 1) * tw);
        const unsigned y0 = (std::max)(2u, (t / kTilesX) * th), y1 = (std::min)(rows_ - 2, (t / kTilesX + 1) * th);
        // Left out: a tile on one of the objects (its box grown by a tile's half).
        bool object = false;
        for (unsigned k = 0; k < object_count && objects && !object; ++k) {
            const auto& o = objects[k];
            const float l = (o.x - 0.5f * o.width) * columns_ - 0.5f * tw, r = (o.x + 0.5f * o.width) * columns_ + 0.5f * tw;
            const float u = (o.y - 0.5f * o.height) * rows_ - 0.5f * th, d = (o.y + 0.5f * o.height) * rows_ + 0.5f * th;
            object = static_cast<float>(x1) > l && static_cast<float>(x0) < r && static_cast<float>(y1) > u && static_cast<float>(y0) < d;
        }
        double sgx = 0, sgy = 0, sxx = 0, sxy = 0, syy = 0; unsigned n = 0;
        for (unsigned y = y0; y < y1; ++y)
            for (unsigned x = x0; x < x1; ++x) {
                const std::uint16_t* c = now + static_cast<std::size_t>(y) * columns_ + x;
                const double ux = 0.5 * kCode * (c[1] - c[-1]), uy = 0.5 * kCode * (c[columns_] - c[-static_cast<std::ptrdiff_t>(columns_)]);
                sgx += ux; sgy += uy; sxx += ux * ux; sxy += ux * uy; syy += uy * uy; ++n;
            }
        if (!n) continue;
        const double mx = sgx / n, my = sgy / n;
        const double a = sxx - n * mx * mx, b = sxy - n * mx * my, d = syy - n * my * my, det = a * d - b * b;
        const double weak = 0.5 * (a + d) - std::sqrt(0.25 * (a - d) * (a - d) + b * b);
        if (weak / n < kMinTexture || !(det > 0)) continue;
        float dx = gx, dy = gy; bool converged = false, lost = false;
        for (unsigned it = 0; it < kIterations && !lost; ++it) {
            double ex = 0, ey = 0, es = 0; unsigned used = 0;
            for (unsigned y = y0; y < y1; ++y)
                for (unsigned x = x0; x < x1; ++x) {
                    float then_value;
                    if (!Sample(then, columns_, rows_, static_cast<float>(x) - dx, static_cast<float>(y) - dy, then_value)) continue;
                    const std::uint16_t* c = now + static_cast<std::size_t>(y) * columns_ + x;
                    const double e = static_cast<double>(c[0]) * kCode - then_value;
                    const double ux = 0.5 * kCode * (c[1] - c[-1]), uy = 0.5 * kCode * (c[columns_] - c[-static_cast<std::ptrdiff_t>(columns_)]);
                    ex += ux * e; ey += uy * e; es += e; ++used;
                }
            if (used < n / 2) { lost = true; break; }
            // The residual's mean (a change of brightness) out: centred gradients against the residual.
            const double bx2 = ex - mx * es, by2 = ey - my * es;
            // now(x) = then(x - d) ~ then(x - d0) - g (d - d0): the step is -H^-1 sum g e.
            const double sx = -(d * bx2 - b * by2) / det, sy = -(a * by2 - b * bx2) / det;
            dx += static_cast<float>(sx); dy += static_cast<float>(sy);
            if (std::fabs(dx - gx) > 4 || std::fabs(dy - gy) > 4) lost = true;
            if (std::fabs(sx) + std::fabs(sy) < 0.002) { converged = true; break; }
        }
        if (lost || !converged) continue;
        on_object[tiles] = object; free += object ? 0u : 1u;
        move_x[tiles] = dx; move_y[tiles] = dy;
        at_x[tiles] = 0.5f * static_cast<float>(x0 + x1 - 1) - ox; at_y[tiles] = 0.5f * static_cast<float>(y0 + y1 - 1) - oy;
        ++tiles;
    }
    // The objects' tiles left out while enough of the background remains (else the fit below sets them apart, if it can).
    if (free >= kMinInliers && free < tiles) {
        unsigned kept = 0;
        for (unsigned i = 0; i < tiles; ++i)
            if (!on_object[i]) { move_x[kept] = move_x[i]; move_y[kept] = move_y[i]; at_x[kept] = at_x[i]; at_y[kept] = at_y[i]; ++kept; }
        tiles = kept;
    }
    result_.tiles = tiles;
    if (tiles < kMinInliers) { last_x_ = last_y_ = 0; previous_ = true; current_ ^= 1u; return result_; }
    // The background: the median motion, then the translation and turn through the tiles near it, twice.
    float scratch[kTiles];
    for (unsigned i = 0; i < tiles; ++i) scratch[i] = move_x[i];
    const float median_x = Median(scratch, tiles);
    for (unsigned i = 0; i < tiles; ++i) scratch[i] = move_y[i];
    const float median_y = Median(scratch, tiles);
    double fx = median_x, fy = median_y, turn = 0;
    bool inlier[kTiles]{}; unsigned count = 0; double rms = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        float deviation[kTiles];
        for (unsigned i = 0; i < tiles; ++i)
            deviation[i] = static_cast<float>(std::hypot(move_x[i] - (fx - turn * at_y[i]), move_y[i] - (fy + turn * at_x[i])));
        for (unsigned i = 0; i < tiles; ++i) scratch[i] = deviation[i];
        const float limit = (std::max)(kInlier, 3.0f * Median(scratch, tiles));
        double n = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sdx = 0, sdy = 0, sxdy = 0, sydx = 0;
        count = 0;
        for (unsigned i = 0; i < tiles; ++i) {
            inlier[i] = deviation[i] <= limit;
            if (!inlier[i]) continue;
            ++count; n += 1; sx += at_x[i]; sy += at_y[i]; sxx += at_x[i] * at_x[i]; syy += at_y[i] * at_y[i];
            sdx += move_x[i]; sdy += move_y[i]; sxdy += at_x[i] * move_y[i]; sydx += at_y[i] * move_x[i];
        }
        if (count < kMinInliers) break;
        // n tx - sy turn = sdx; n ty + sx turn = sdy; -sy tx + sx ty + (sxx + syy) turn = sxdy - sydx.
        const double m[3][3]{{n, 0, -sy}, {0, n, sx}, {-sy, sx, sxx + syy}}, v[3]{sdx, sdy, sxdy - sydx};
        const auto det3 = [](const double (&q)[3][3]) {
            return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
        };
        const double det = det3(m);
        if (!(std::fabs(det) > 1e-9)) break;
        double solved[3];
        for (unsigned c = 0; c < 3; ++c) {
            double q[3][3]; for (unsigned r = 0; r < 3; ++r) for (unsigned k = 0; k < 3; ++k) q[r][k] = k == c ? v[r] : m[r][k];
            solved[c] = det3(q) / det;
        }
        fx = solved[0]; fy = solved[1]; turn = solved[2];
    }
    if (count >= kMinInliers) {
        for (unsigned i = 0; i < tiles; ++i)
            if (inlier[i]) { const double ex = move_x[i] - (fx - turn * at_y[i]), ey = move_y[i] - (fy + turn * at_x[i]); rms += ex * ex + ey * ey; }
        result_.valid = true; result_.inliers = count;
        result_.dx = static_cast<float>(fx * scale_); result_.dy = static_cast<float>(fy * scale_); result_.turn = static_cast<float>(turn);
        result_.residual = static_cast<float>(std::sqrt(rms / count) * scale_);
        last_x_ = static_cast<float>(fx); last_y_ = static_cast<float>(fy); ++measured_;
    } else last_x_ = last_y_ = 0;
    current_ ^= 1u;
    return result_;
}
EgoMotion VirtualImu::Ego() const noexcept {
    EgoMotion e;
    if (!result_.valid || !(focal_ > 0)) return e;
    e.yaw = static_cast<float>(-result_.dx / focal_); e.pitch = static_cast<float>(result_.dy / focal_); e.roll = -result_.turn; e.valid = true;
    return e;
}
}
