#include "media/video/Vibrometry.hpp"
#include <algorithm>
#include <cmath>

namespace media::video {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kSecond = 10000000.0;
// Below this a detrended signal is flat: codes (the means), samples (the displacement).
constexpr double kFloor[VibrometryResult::kChannels]{0.01, 0.01, 0.01, 0.001, 0.001};
float Confidence(double snr) noexcept {
    return static_cast<float>(std::clamp((snr - Vibrometry::kSnrFloor) / Vibrometry::kSnrSpan, 0.0, 1.0));
}
}

void Vibrometry::Reset() noexcept {
    head_ = count_ = since_ = 0; referenced_ = false; shift_x_ = shift_y_ = 0; restarts_ = 0; region_ = {}; result_ = {}; follow_ = false;
}
bool Vibrometry::Begin(AreaRect r, std::int64_t pts, bool follow) noexcept {
    if (!(r.right - r.left >= 4) || !(r.bottom - r.top >= 4)) return false;
    const double w = r.right - r.left, h = r.bottom - r.top;
    if (count_) {
        const double pw = region_.right - region_.left, ph = region_.bottom - region_.top;
        // Following, the region may go anywhere a step at a time; a jump is another target (or the tracker finding it again).
        const double reach = follow ? 0.5 : 0.25;
        const bool moved = std::fabs((r.left + r.right - region_.left - region_.right) / 2) > reach * pw ||
                           std::fabs((r.top + r.bottom - region_.top - region_.bottom) / 2) > reach * ph ||
                           std::fabs(w - pw) > 0.1 * pw || std::fabs(h - ph) > 0.1 * ph || follow != follow_;
        if (moved || pts <= times_[(head_ + kWindow - 1) % kWindow]) {
            head_ = count_ = since_ = 0; referenced_ = false; shift_x_ = shift_y_ = 0; result_ = {}; ++restarts_;
        }
    }
    region_ = r; follow_ = follow;
    return true;
}
void Vibrometry::Push(const Planes420& p, AreaRect r, std::int64_t pts, bool follow) noexcept {
    if (!p.y || !Begin(r, pts, follow)) return; // Chroma may be absent (a luma grid): flat.
    const double w = r.right - r.left, h = r.bottom - r.top;
    const double luma = AreaMean(p.y, p.luma_stride, p.width, p.height, r);
    if (luma < 0) return;
    const AreaRect c{r.left / 2, r.top / 2, r.right / 2, r.bottom / 2};
    const unsigned cw = (p.width + 1) / 2, ch = (p.height + 1) / 2;
    const float cb = p.cb ? static_cast<float>(AreaMean(p.cb, p.chroma_stride, cw, ch, c)) : 0.0f;
    const float cr = p.cr ? static_cast<float>(AreaMean(p.cr, p.chroma_stride, cw, ch, c)) : 0.0f;
    const double cell_w = w / kCells, cell_h = h / kCells;
    for (unsigned j = 0; j < kCells; ++j)
        for (unsigned i = 0; i < kCells; ++i)
            cells_[j * kCells + i] = static_cast<float>(AreaMean(p.y, p.luma_stride, p.width, p.height,
                                                                 {r.left + i * cell_w, r.top + j * cell_h, r.left + (i + 1) * cell_w, r.top + (j + 1) * cell_h}));
    Record(static_cast<float>(luma), cb, cr, r, pts, [&] { Reference(p, r); });
}
void Vibrometry::Push(const VibrometrySample& s, AreaRect r, std::int64_t pts, bool follow) noexcept {
    if (!Begin(r, pts, follow)) return;
    std::copy(s.cells, s.cells + kCells * kCells, cells_);
    Record(s.luma, s.cb, s.cr, r, pts, [&] {
        // The measured cells and their moves are the reference's (Reference's numbers, from the GPU).
        for (unsigned k = 0; k < kCells * kCells; ++k) { reference_[k] = cells_[k]; gx_[k] = s.right[k] - s.left[k]; gy_[k] = s.below[k] - s.above[k]; }
        referenced_ = true;
    });
}
template <class Refer>
void Vibrometry::Record(float luma, float cb, float cr, AreaRect r, std::int64_t pts, const Refer& reference) noexcept {
    float v[VibrometryResult::kChannels]{};
    v[VibrometryResult::kLuma] = luma; v[VibrometryResult::kCb] = cb; v[VibrometryResult::kCr] = cr;
    // The content's displacement: the region's cell means against the reference grid, one Lucas-Kanade step (the shifts
    // measured are small beside a cell: the linear model holds), in cells, then samples.
    const double cell_w = (r.right - r.left) / kCells, cell_h = (r.bottom - r.top) / kCells;
    double dx = 0, dy = 0; // Samples.
    if (!referenced_) reference();
    else {
        double a = 0, b = 0, d = 0, ex = 0, ey = 0; unsigned used = 0;
        for (unsigned k = 0; k < kCells * kCells; ++k) {
            if (cells_[k] < 0 || reference_[k] < 0) continue;
            const double gx = gx_[k], gy = gy_[k], it = static_cast<double>(cells_[k]) - reference_[k];
            a += gx * gx; b += gx * gy; d += gy * gy; ex += gx * it; ey += gy * it; ++used;
        }
        // Content moved by s: now(x) = ref(x - s) ~ ref(x) - g.s, so the difference is -g.s. Textured enough in both axes
        // (a mean squared gradient of 0.01 codes per sample, the system well conditioned), else no displacement.
        const double det = a * d - b * b;
        if (used && (a + d) / used > 0.01 && det > 1e-3 * (a + d) * (a + d)) { dx = -(d * ex - b * ey) / det; dy = -(a * ey - b * ex) / det; }
        if (std::fabs(dx) > 0.5 * cell_w || std::fabs(dy) > 0.5 * cell_h) {
            // Half a cell: a new reference here, the series continuing from where it stands.
            shift_x_ += dx; shift_y_ += dy; dx = dy = 0;
            reference();
        }
    }
    v[VibrometryResult::kShiftX] = static_cast<float>(shift_x_ + dx);
    v[VibrometryResult::kShiftY] = static_cast<float>(shift_y_ + dy);
    for (unsigned k = 0; k < VibrometryResult::kChannels; ++k) values_[k][head_] = v[k];
    times_[head_] = pts; head_ = (head_ + 1) % kWindow; count_ = (std::min)(count_ + 1, kWindow);
    if (++since_ >= kEvery) { since_ = 0; Analyse(); }
}
void Vibrometry::Reference(const Planes420& p, AreaRect r) noexcept {
    // The cells as they are now, and each one's mean moved half a sample either way (its derivative under a shift).
    const double cell_w = (r.right - r.left) / kCells, cell_h = (r.bottom - r.top) / kCells;
    const auto mean = [&](double x0, double y0, double x1, double y1) { return AreaMean(p.y, p.luma_stride, p.width, p.height, {x0, y0, x1, y1}); };
    for (unsigned j = 0; j < kCells; ++j)
        for (unsigned i = 0; i < kCells; ++i) {
            const unsigned k = j * kCells + i;
            const double x0 = r.left + i * cell_w, y0 = r.top + j * cell_h, x1 = x0 + cell_w, y1 = y0 + cell_h;
            reference_[k] = cells_[k];
            const double right = mean(x0 + 0.5, y0, x1 + 0.5, y1), left = mean(x0 - 0.5, y0, x1 - 0.5, y1);
            const double below = mean(x0, y0 + 0.5, x1, y1 + 0.5), above = mean(x0, y0 - 0.5, x1, y1 - 0.5);
            const bool inside = right >= 0 && left >= 0 && below >= 0 && above >= 0 && x0 >= 0.5 && y0 >= 0.5 && x1 + 0.5 <= p.width && y1 + 0.5 <= p.height;
            gx_[k] = inside ? static_cast<float>(right - left) : 0.0f; gy_[k] = inside ? static_cast<float>(below - above) : 0.0f;
        }
    referenced_ = true;
}
double Vibrometry::Power(unsigned channel, double hz) const noexcept {
    double re = 0, im = 0;
    for (unsigned k = 0; k < count_; ++k) {
        const double phase = 2 * kPi * hz * t_[k];
        re += weighted_[channel][k] * std::cos(phase); im -= weighted_[channel][k] * std::sin(phase);
    }
    return re * re + im * im;
}
Vibrometry::Peak Vibrometry::Refine(unsigned channel, double hz, double step) const noexcept {
    // Golden-section search for the maximum within a bin of the coarse peak (inside the window's main lobe: one maximum).
    constexpr double kGolden = 0.6180339887498949;
    double lo = hz - step, hi = hz + step;
    double x1 = hi - kGolden * (hi - lo), x2 = lo + kGolden * (hi - lo);
    double p1 = Power(channel, x1), p2 = Power(channel, x2);
    for (unsigned i = 0; i < 24; ++i) {
        if (p1 < p2) { lo = x1; x1 = x2; p1 = p2; x2 = lo + kGolden * (hi - lo); p2 = Power(channel, x2); }
        else { hi = x2; x2 = x1; p2 = p1; x1 = hi - kGolden * (hi - lo); p1 = Power(channel, x1); }
    }
    Peak peak; peak.hz = p1 > p2 ? x1 : x2; peak.power = (std::max)(p1, p2);
    return peak;
}
void Vibrometry::Analyse() noexcept {
    VibrometryResult out{};
    const unsigned n = count_, first = (head_ + kWindow - n) % kWindow;
    const std::int64_t t0 = times_[first], t1 = times_[(head_ + kWindow - 1) % kWindow];
    const double span = static_cast<double>(t1 - t0) / kSecond;
    out.samples = n; out.seconds = static_cast<float>(span); out.rate = n > 1 && span > 0 ? static_cast<float>((n - 1) / span) : 0;
    const double high = (std::min)(static_cast<double>(kHigh), 0.45 * out.rate);
    if (n < 32 || span < kMinSeconds || high < kLow + 0.5) { result_ = out; return; }
    out.known = true;
    // The window in time order, each signal detrended (least-squares line) and weighted by a Hann window over the span.
    double tm = 0;
    for (unsigned k = 0; k < n; ++k) { t_[k] = static_cast<double>(times_[(first + k) % kWindow] - t0) / kSecond; tm += t_[k]; }
    tm /= n;
    double stt = 0; for (unsigned k = 0; k < n; ++k) stt += (t_[k] - tm) * (t_[k] - tm);
    weight_sum_ = 0;
    bool live[VibrometryResult::kChannels]{};
    for (unsigned c = 0; c < VibrometryResult::kChannels; ++c) {
        double xm = 0; for (unsigned k = 0; k < n; ++k) xm += values_[c][(first + k) % kWindow];
        xm /= n;
        double sxt = 0; for (unsigned k = 0; k < n; ++k) sxt += (t_[k] - tm) * (values_[c][(first + k) % kWindow] - xm);
        const double slope = stt > 0 ? sxt / stt : 0;
        if (follow_) {
            // A target crossing the scene: its own moving mean (a centred box of kTrendSeconds) instead of the line.
            const double half = kTrendSeconds / 2.0;
            unsigned lo = 0, hi = 0; double sum = 0;
            for (unsigned k = 0; k < n; ++k) {
                while (hi < n && t_[hi] <= t_[k] + half) sum += values_[c][(first + hi++) % kWindow];
                while (t_[lo] < t_[k] - half) sum -= values_[c][(first + lo++) % kWindow];
                trend_[k] = sum / (hi - lo);
            }
        }
        double energy = 0; weight_sum_ = 0;
        for (unsigned k = 0; k < n; ++k) {
            double residual = values_[c][(first + k) % kWindow] - (follow_ ? trend_[k] : xm + slope * (t_[k] - tm));
            // The luma in its share of the light on the target (codes at the window's mean): the same pulse in shade and in light.
            if (follow_ && c == VibrometryResult::kLuma && trend_[k] > 1) residual *= xm / trend_[k];
            const double hann = 0.5 - 0.5 * std::cos(2 * kPi * t_[k] / span);
            weighted_[c][k] = hann * residual; weight_sum_ += hann; energy += residual * residual;
        }
        live[c] = std::sqrt(energy / n) >= kFloor[c];
    }
    // The coarse spectrum of every live signal on one grid (each sample's phasor turned bin by bin, in double).
    double step = 1 / (2 * span);
    unsigned bins = static_cast<unsigned>((high - kLow) / step) + 1;
    if (bins > kMaxBins) { bins = kMaxBins; step = (high - kLow) / (kMaxBins - 1); }
    double zr[kWindow], zi[kWindow], sr[kWindow], si[kWindow];
    for (unsigned k = 0; k < n; ++k) {
        zr[k] = std::cos(2 * kPi * kLow * t_[k]); zi[k] = -std::sin(2 * kPi * kLow * t_[k]);
        sr[k] = std::cos(2 * kPi * step * t_[k]); si[k] = -std::sin(2 * kPi * step * t_[k]);
    }
    for (unsigned b = 0; b < bins; ++b) {
        double re[VibrometryResult::kChannels]{}, im[VibrometryResult::kChannels]{};
        for (unsigned k = 0; k < n; ++k) {
            for (unsigned c = 0; c < VibrometryResult::kChannels; ++c) { re[c] += weighted_[c][k] * zr[k]; im[c] += weighted_[c][k] * zi[k]; }
            const double r2 = zr[k] * sr[k] - zi[k] * si[k], i2 = zr[k] * si[k] + zi[k] * sr[k];
            zr[k] = r2; zi[k] = i2;
        }
        for (unsigned c = 0; c < VibrometryResult::kChannels; ++c) spectrum_[c][b] = static_cast<float>(re[c] * re[c] + im[c] * im[c]);
    }
    // Each live signal's strongest bin over the band and within the vital band, by its power over the band's median.
    Peak best, vital; unsigned best_c = 0, vital_c = 0;
    for (unsigned c = 0; c < VibrometryResult::kChannels; ++c) {
        if (!live[c]) continue;
        float sorted[kMaxBins];
        std::copy(spectrum_[c], spectrum_[c] + bins, sorted);
        std::nth_element(sorted, sorted + bins / 2, sorted + bins);
        const double median = (std::max)(static_cast<double>(sorted[bins / 2]), 1e-30);
        unsigned top = 0, vtop = bins;
        for (unsigned b = 0; b < bins; ++b) {
            if (spectrum_[c][b] > spectrum_[c][top]) top = b;
            const double f = kLow + b * step;
            if (f >= kVitalLow && f <= kVitalHigh && (vtop == bins || spectrum_[c][b] > spectrum_[c][vtop])) vtop = b;
        }
        if (spectrum_[c][top] / median > best.snr) {
            auto peak = Refine(c, kLow + top * step, step); peak.snr = peak.power / median;
            if (peak.snr > best.snr) { best = peak; best_c = c; }
        }
        if (vtop < bins && spectrum_[c][vtop] / median > vital.snr) {
            auto peak = Refine(c, kLow + vtop * step, step); peak.snr = peak.power / median;
            if (peak.hz >= kVitalLow && peak.hz <= kVitalHigh && peak.snr > vital.snr) { vital = peak; vital_c = c; }
        }
    }
    // A vital component where a harmonic of a stronger one folds to at the picture rate is that harmonic, not a pulse.
    if (vital.hz > 0 && best.snr > vital.snr && std::fabs(best.hz - vital.hz) > 2 * step)
        for (unsigned k = 2; k <= 6; ++k) {
            double folded = std::fmod(k * best.hz, static_cast<double>(out.rate));
            if (folded > out.rate / 2.0) folded = out.rate - folded;
            if (std::fabs(folded - vital.hz) < 2 * step) { vital = {}; break; }
        }
    out.snr = static_cast<float>(best.snr); out.vital_snr = static_cast<float>(vital.snr);
    out.confidence = Confidence(best.snr); out.vital_confidence = Confidence(vital.snr);
    if (out.confidence > 0) {
        out.vibration_hz = static_cast<float>(best.hz); out.channel = best_c;
        out.amplitude = weight_sum_ > 0 ? static_cast<float>(2 * std::sqrt(best.power) / weight_sum_) : 0;
    }
    if (out.vital_confidence > 0) { out.vital_bpm = static_cast<float>(60 * vital.hz); out.vital_channel = vital_c; }
    result_ = out;
}
void TargetPath::Push(std::int64_t pts, double x, double y) noexcept {
    if (count_ && pts <= t_[(head_ + kPoints - 1) % kPoints]) Reset(); // Time went back: another timeline.
    t_[head_] = pts; x_[head_] = x; y_[head_] = y;
    head_ = (head_ + 1) % kPoints; count_ = (std::min)(count_ + 1, kPoints);
}
bool TargetPath::At(std::int64_t pts, double& x, double& y) const noexcept {
    if (!count_) return false;
    const unsigned newest = (head_ + kPoints - 1) % kPoints;
    // The points within kSeconds of the newest, the line through them (times relative to the newest, in seconds).
    double n = 0, st = 0, stt = 0, sx = 0, sy = 0, stx = 0, sty = 0;
    for (unsigned k = 0; k < count_; ++k) {
        const unsigned i = (newest + kPoints - k) % kPoints;
        const double t = static_cast<double>(t_[i] - t_[newest]) / 1e7;
        if (t < -kSeconds) break;
        n += 1; st += t; stt += t * t; sx += x_[i]; sy += y_[i]; stx += t * x_[i]; sty += t * y_[i];
    }
    const double d = n * stt - st * st;
    if (n < 2 || !(d > 1e-12)) { x = x_[newest]; y = y_[newest]; return true; }
    const double when = static_cast<double>(pts - t_[newest]) / 1e7;
    const double bx = (n * stx - st * sx) / d, by = (n * sty - st * sy) / d;
    x = (sx - bx * st) / n + bx * when; y = (sy - by * st) / n + by * when;
    return true;
}
}
