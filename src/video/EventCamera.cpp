#include "media/video/EventCamera.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::video {
namespace {
unsigned ScaleFor(unsigned width, unsigned height) noexcept {
    const unsigned across = (width + EventCamera::kMaxWidth - 1) / EventCamera::kMaxWidth, down = (height + EventCamera::kMaxHeight - 1) / EventCamera::kMaxHeight;
    return (std::max)({across, down, 1u});
}
constexpr std::uint64_t kPlaneAlign = 64;
std::uint64_t PlaneBytes(unsigned w, unsigned h) noexcept { return (std::uint64_t{w} * h * sizeof(std::int16_t) + kPlaneAlign - 1) / kPlaneAlign * kPlaneAlign; }
// A ring entry: x (bits 0-9), y (10-18), ON (19), contrast (20-27), the timestamp's low 36 bits (28-63).
constexpr unsigned kTimeShift = 28;
constexpr std::uint64_t kTimeMask = (std::uint64_t{1} << 36) - 1;
// Catmull-Rom (bicubic) sample of the codes at (x, y), the edges' samples repeated, clamped to the codes' range: what a
// sub-sample shift of band-limited content looks like, within a few hundredths of a code where bilinear is a code off at an
// edge (and an edge 160 codes high fires at a tenth of a sample's error). `unit`: a sample's code in sixteenths.
template <class Sample>
std::int32_t Cubic(const Sample* plane, std::size_t stride, unsigned unit, unsigned w, unsigned h, double x, double y) noexcept {
    const auto weights = [](double t, double* k) {
        const double t2 = t * t, t3 = t2 * t;
        k[0] = 0.5 * (-t3 + 2 * t2 - t); k[1] = 0.5 * (3 * t3 - 5 * t2 + 2); k[2] = 0.5 * (-3 * t3 + 4 * t2 + t); k[3] = 0.5 * (t3 - t2);
    };
    const auto x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    double kx[4], ky[4]; weights(x - x0, kx); weights(y - y0, ky);
    const int last_x = static_cast<int>(w) - 1, last_y = static_cast<int>(h) - 1;
    double sum = 0;
    for (int b = 0; b < 4; ++b) {
        const auto* row = plane + static_cast<std::size_t>(std::clamp(y0 - 1 + b, 0, last_y)) * stride;
        double r = 0;
        for (int a = 0; a < 4; ++a) r += kx[a] * (row[std::clamp(x0 - 1 + a, 0, last_x)] * unit);
        sum += ky[b] * r;
    }
    return static_cast<std::int32_t>(std::clamp(std::lround(sum), 0L, 4095L));
}
}

std::uint64_t EventCamera::Bytes(unsigned width, unsigned height, unsigned ring) noexcept {
    const unsigned s = ScaleFor(width, height), w = width / s, h = height / s;
    return (s > 1 ? 3 : 2) * PlaneBytes(w, h) + std::uint64_t{ring} * sizeof(std::uint64_t) + 4 * kPlaneAlign;
}
bool EventCamera::Configure(core::LinearMemoryArena& arena, unsigned width, unsigned height, unsigned ring) noexcept {
    ref_ = nullptr;
    if (width < 16 || height < 16 || ring < 256 || (ring & (ring - 1))) return false;
    scale_ = ScaleFor(width, height); width_ = width; height_ = height; sw_ = width / scale_; sh_ = height / scale_; ring_size_ = ring;
    const auto plane = static_cast<std::size_t>(PlaneBytes(sw_, sh_));
    auto* ref = static_cast<std::int16_t*>(arena.Allocate(plane, kPlaneAlign));
    scratch_ = static_cast<std::int16_t*>(arena.Allocate(plane, kPlaneAlign));
    now_ = scale_ > 1 ? static_cast<std::uint16_t*>(arena.Allocate(plane, kPlaneAlign)) : nullptr;
    ring_ = static_cast<std::uint64_t*>(arena.Allocate(std::size_t{ring} * sizeof(std::uint64_t), kPlaneAlign));
    if (!ref || !scratch_ || (scale_ > 1 && !now_) || !ring_) return false;
    // log((Y' - 16) / 219 linearised with gamma 2.2, + 1/64), Q10, for each sixteenth of a code.
    for (unsigned v = 0; v < 4096; ++v) {
        const double linear = std::pow(std::clamp((v / 16.0 - 16) / 219, 0.0, 1.0), 2.2);
        log_[v] = static_cast<std::int16_t>(std::lround(1024 * std::log(linear + 1.0 / 64)));
    }
    armed_ = false; tx_ = ty_ = theta_ = 0; emitted_ = dropped_ = 0; rebases_ = pictures_ = last_ = 0;
    produced_ = 0; write_.store(0, std::memory_order_relaxed); read_.store(0, std::memory_order_relaxed); newest_us_.store(0, std::memory_order_relaxed);
    ref_ = ref;
    return true;
}
void EventCamera::SetOptions(const EventCameraOptions& o) noexcept {
    options_ = o;
    options_.contrast_threshold = std::clamp(o.contrast_threshold, 0.01f, 2.0f);
    options_.max_per_pixel = std::clamp(o.max_per_pixel, 1u, 16u);
    armed_ = false; // From the next picture, with these.
}
std::int32_t EventCamera::CodeAt(double x, double y) const noexcept {
    return now_ ? Cubic(now_, sw_, 1u, sw_, sh_, x, y) : Cubic(luma_, luma_stride_, 16u, sw_, sh_, x, y);
}
void EventCamera::Decimate(const std::uint8_t* luma, unsigned stride) noexcept {
    const unsigned s = scale_, area = s * s;
    for (unsigned j = 0; j < sh_; ++j) {
        std::uint16_t* out = now_ + static_cast<std::size_t>(j) * sw_;
        for (unsigned i = 0; i < sw_; ++i) {
            unsigned sum = 0;
            for (unsigned y = 0; y < s; ++y) { const auto* row = luma + static_cast<std::size_t>(j * s + y) * stride + i * s; for (unsigned x = 0; x < s; ++x) sum += row[x]; }
            out[i] = static_cast<std::uint16_t>((sum * 16 + area / 2) / area);
        }
    }
}
void EventCamera::Rebase() noexcept {
    ++rebases_;
    if (std::fabs(theta_) > kRebaseRoll) {
        // A turn: the references start over from the picture (this picture fires nothing; a resampled reference would be
        // blurred, and its sharp edges would fire).
        for (unsigned j = 0; j < sh_; ++j) for (unsigned i = 0; i < sw_; ++i) ref_[static_cast<std::size_t>(j) * sw_ + i] = log_[Code(i, j)];
        tx_ = ty_ = theta_ = 0;
        return;
    }
    // A shift: the scene's frame moves by whole pixels m toward the camera's (q' = q - m), its references copied, not
    // resampled; p = R(theta)(q' - c) + c + t' with t' = t + R(theta) m, m = round(-R(-theta) t). A pixel whose old place is
    // outside the frame (content that has come into view) takes the picture where it lies now.
    const double c = std::cos(theta_), s = std::sin(theta_);
    const long mx = std::lround(-(c * tx_ + s * ty_)), my = std::lround(-(-s * tx_ + c * ty_));
    tx_ += c * mx - s * my; ty_ += s * mx + c * my;
    const double cx = (sw_ - 1) * 0.5, cy = (sh_ - 1) * 0.5;
    for (unsigned j = 0; j < sh_; ++j)
        for (unsigned i = 0; i < sw_; ++i) {
            const std::size_t k = static_cast<std::size_t>(j) * sw_ + i;
            const long si = static_cast<long>(i) + mx, sj = static_cast<long>(j) + my;
            if (si >= 0 && sj >= 0 && si < static_cast<long>(sw_) && sj < static_cast<long>(sh_)) { scratch_[k] = ref_[static_cast<std::size_t>(sj) * sw_ + static_cast<std::size_t>(si)]; continue; }
            const double qx = i - cx, qy = j - cy;
            const double px = std::clamp(c * qx - s * qy + cx + tx_, 0.0, sw_ - 1.0), py = std::clamp(s * qx + c * qy + cy + ty_, 0.0, sh_ - 1.0);
            scratch_[k] = log_[CodeAt(px, py)];
        }
    std::swap(ref_, scratch_);
}
std::uint32_t EventCamera::Push(const std::uint8_t* luma, unsigned stride, std::int64_t pts, float dx, float dy, float roll) noexcept {
    if (!ref_ || !luma) return 0;
    if (now_) Decimate(luma, stride); else { luma_ = luma; luma_stride_ = stride; }
    ++pictures_; last_ = 0;
    if (!armed_ || pts <= last_pts_) {
        for (unsigned j = 0; j < sh_; ++j) for (unsigned i = 0; i < sw_; ++i) ref_[static_cast<std::size_t>(j) * sw_ + i] = log_[Code(i, j)];
        tx_ = ty_ = theta_ = 0; armed_ = true; last_pts_ = pts;
        return 0;
    }
    if (options_.stabilized) {
        // The scene moved by d and turned by r about the centre: p' = R(r)(p - c) + c + d, so t' = R(r) t + d, theta' = theta + r.
        const double r = roll, c = std::cos(r), s = std::sin(r);
        const double x = c * tx_ - s * ty_ + dx / scale_, y = s * tx_ + c * ty_ + dy / scale_;
        tx_ = x; ty_ = y; theta_ += r;
        if (std::hypot(tx_, ty_) > kRebase || std::fabs(theta_) > kRebaseRoll) Rebase();
    }
    const auto threshold = static_cast<std::int32_t>((std::max)(1L, std::lround(options_.contrast_threshold * 1024)));
    const std::int32_t most = static_cast<std::int32_t>(options_.max_per_pixel);
    const double cx = (sw_ - 1) * 0.5, cy = (sh_ - 1) * 0.5, c = std::cos(theta_), s = std::sin(theta_);
    const bool still = !options_.stabilized || (tx_ == 0 && ty_ == 0 && theta_ == 0);
    // Pass 1: each reference pixel against the picture where its scene point lies now; the crossings counted per slice. While
    // the camera's motion is taken out, a pixel's change is first reduced by what a misregistration of kRegistration pixels
    // makes of its reference's slope (an edge 160 codes high is ~1.4 log units a pixel: a tenth of a pixel would fire it).
    std::uint32_t counts[kSlices]{};
    for (unsigned j = 0; j < sh_; ++j)
        for (unsigned i = 0; i < sw_; ++i) {
            const std::size_t k = static_cast<std::size_t>(j) * sw_ + i;
            std::int32_t v, guard = 0;
            if (still) v = log_[Code(i, j)];
            else {
                const double qx = i - cx, qy = j - cy, px = c * qx - s * qy + cx + tx_, py = s * qx + c * qy + cy + ty_;
                if (px < 0 || py < 0 || px > sw_ - 1 || py > sh_ - 1) { scratch_[k] = 0; continue; }
                v = log_[CodeAt(px, py)];
                const std::int32_t gx = i > 0 && i + 1 < sw_ ? std::abs(ref_[k + 1] - ref_[k - 1]) : 0, gy = j > 0 && j + 1 < sh_ ? std::abs(ref_[k + sw_] - ref_[k - sw_]) : 0;
                guard = static_cast<std::int32_t>(kRegistration * 0.5 * (std::max)(gx, gy));
            }
            const std::int32_t raw = v - ref_[k], d = raw < 0 ? (std::min)(raw + guard, 0) : (std::max)(raw - guard, 0), magnitude = d < 0 ? -d : d;
            scratch_[k] = static_cast<std::int16_t>(d);
            if (magnitude < threshold) continue;
            const std::int32_t crossings = magnitude / threshold, n = (std::min)(crossings, most);
            for (std::int32_t m = 1; m <= n; ++m) {
                const auto slice = static_cast<unsigned>((std::int64_t{kSlices} * m * threshold + magnitude - 1) / magnitude) - 1;
                ++counts[(std::min)(slice, kSlices - 1)];
            }
            // The reference moves by the crossings fired (a DVS pixel resets at each); past the cap, to the picture.
            ref_[k] = static_cast<std::int16_t>(crossings > most ? v : ref_[k] + (d < 0 ? -n : n) * threshold);
        }
    // Room: the earliest slices first (the one that does not fit, its first events in raster order).
    const std::uint64_t free = ring_size_ - (produced_ - read_.load(std::memory_order_acquire));
    std::uint32_t start[kSlices]{}, quota[kSlices]{}, total = 0;
    std::uint64_t kept = 0;
    for (unsigned sl = 0; sl < kSlices; ++sl) {
        start[sl] = total; total += counts[sl];
        quota[sl] = static_cast<std::uint32_t>(start[sl] >= free ? 0 : (std::min)(std::uint64_t{counts[sl]}, free - start[sl]));
        kept += quota[sl];
    }
    // Pass 2: each event at its place in time order (raster order within a slice), stamped with its slice's end.
    std::int64_t stamps[kSlices];
    for (unsigned sl = 0; sl < kSlices; ++sl) stamps[sl] = (last_pts_ + (pts - last_pts_) * static_cast<std::int64_t>(sl + 1) / kSlices) / 10;
    std::uint32_t fill[kSlices]{};
    if (kept)
        for (unsigned j = 0; j < sh_; ++j)
            for (unsigned i = 0; i < sw_; ++i) {
                const std::int32_t d = scratch_[static_cast<std::size_t>(j) * sw_ + i], magnitude = d < 0 ? -d : d;
                if (magnitude < threshold) continue;
                const std::int32_t n = (std::min)(magnitude / threshold, most);
                for (std::int32_t m = 1; m <= n; ++m) {
                    const auto slice = (std::min)(static_cast<unsigned>((std::int64_t{kSlices} * m * threshold + magnitude - 1) / magnitude) - 1, kSlices - 1);
                    if (fill[slice] >= quota[slice]) continue;
                    const std::uint64_t contrast = static_cast<std::uint64_t>((std::min)(magnitude / 16, 255));
                    ring_[(produced_ + start[slice] + fill[slice]++) & (ring_size_ - 1)] = std::uint64_t{i} | std::uint64_t{j} << 10 | (d < 0 ? std::uint64_t{0} : std::uint64_t{1} << 19) |
                                                                                           contrast << 20 | (static_cast<std::uint64_t>(stamps[slice]) & kTimeMask) << kTimeShift;
                }
            }
    produced_ += kept;
    if (kept) newest_us_.store(stamps[kSlices - 1], std::memory_order_relaxed);
    write_.store(produced_, std::memory_order_release);
    emitted_ += kept; dropped_ += total - kept; last_ = static_cast<std::uint32_t>(kept); last_pts_ = pts;
    return last_;
}
std::uint32_t EventCamera::Poll(EventSpike* out, std::uint32_t capacity) noexcept {
    if (!ring_ || !out) return 0;
    const std::uint64_t r = read_.load(std::memory_order_relaxed), w = write_.load(std::memory_order_acquire);
    const auto n = static_cast<std::uint32_t>((std::min)(w - r, std::uint64_t{capacity}));
    const std::int64_t newest = newest_us_.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint64_t e = ring_[(r + i) & (ring_size_ - 1)];
        // The timestamp nearest the newest one with these low bits (a seek back leaves older events with later times).
        auto apart = static_cast<std::int64_t>(((e >> kTimeShift) - static_cast<std::uint64_t>(newest)) & kTimeMask);
        if (apart >= std::int64_t{1} << 35) apart -= std::int64_t{1} << 36;
        out[i] = {static_cast<std::uint16_t>(e & 1023u), static_cast<std::uint16_t>((e >> 10) & 511u), (e >> 19) & 1u ? std::int8_t{1} : std::int8_t{-1},
                  static_cast<std::uint8_t>((e >> 20) & 255u), 0, newest + apart};
    }
    read_.store(r + n, std::memory_order_release);
    return n;
}
}
