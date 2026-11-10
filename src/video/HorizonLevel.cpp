#include "media/video/HorizonLevel.hpp"
#include "SimdLevel.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace media::video {
namespace {
constexpr float kEnergy = 0.005f, kCoherence = 0.85f; // HdrToneMap.hlsl kHorizonEnergy, kHorizonCoherence.
constexpr unsigned kAcross = 80;                    // kHorizonAcross.
// The structure tensor of a block's centre 8 x 8 texels in integer units (2026-10-31): the Scharr gradients of the 8-bit codes
// themselves, 3 (a - c) + 10 (d - f) + 3 (g - i), are 32 x 255 = 8160 times the shader's (code / 255, the kernel / 32), so the
// tensor's sums are kUnit times its. Every term is exact: a gradient fits 16 bits (16 x 255 = 4080 each way) and a sum of 64
// squares 31 (64 x 4080^2 = 1,065,369,600), so the scalar loop, SSE2 and AVX2 give the same three integers, and the block's
// line (the arithmetic below, one scalar function) the same words, bit for bit. Separable: per row of the 10 x 10 window its
// horizontal difference c - a and smoothing 3 a + 10 b + 3 c at the 8 centre columns, then per centre row gx = 3 (dx above +
// dx below) + 10 dx, gy = sx below - sx above.
constexpr double kUnit = 8160.0 * 8160.0;
struct Tensor { std::int32_t xx = 0, yy = 0, xy = 0; };
// `p`: the window's top-left texel; `step` bytes between texels in a row.
Tensor TensorScalar(const std::uint8_t* p, unsigned stride, unsigned step) noexcept {
    int dx[10][8], sx[10][8];
    for (unsigned j = 0; j < 10; ++j) {
        const std::uint8_t* row = p + static_cast<std::size_t>(j) * stride;
        for (unsigned x = 0; x < 8; ++x) {
            const int a = row[x * step], b = row[(x + 1) * step], c = row[(x + 2) * step];
            dx[j][x] = c - a; sx[j][x] = 3 * (a + c) + 10 * b;
        }
    }
    Tensor t;
    for (unsigned y = 1; y < 9; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            const int gx = 3 * (dx[y - 1][x] + dx[y + 1][x]) + 10 * dx[y][x], gy = sx[y + 1][x] - sx[y - 1][x];
            t.xx += gx * gx; t.yy += gy * gy; t.xy += gx * gy;
        }
    return t;
}
#if MEDIA_SURVEY_SSE2
std::int32_t Lanes(__m128i v) noexcept {
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4E)); v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xB1));
    return _mm_cvtsi128_si32(v);
}
// A row of the window's 8 centre columns a register (8 x 16 bits); the squares and products summed in pairs (madd) into
// 32-bit lanes.
Tensor TensorSse2(const std::uint8_t* p, unsigned stride) noexcept {
    const __m128i zero = _mm_setzero_si128(), three = _mm_set1_epi16(3), ten = _mm_set1_epi16(10);
    __m128i dx[10], sx[10];
    for (unsigned j = 0; j < 10; ++j) {
        const std::uint8_t* row = p + static_cast<std::size_t>(j) * stride;
        const __m128i a = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(row)), zero);
        const __m128i b = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(row + 1)), zero);
        const __m128i c = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(row + 2)), zero);
        dx[j] = _mm_sub_epi16(c, a); sx[j] = _mm_add_epi16(_mm_mullo_epi16(_mm_add_epi16(a, c), three), _mm_mullo_epi16(b, ten));
    }
    __m128i xx = zero, yy = zero, xy = zero;
    for (unsigned y = 1; y < 9; ++y) {
        const __m128i gx = _mm_add_epi16(_mm_mullo_epi16(_mm_add_epi16(dx[y - 1], dx[y + 1]), three), _mm_mullo_epi16(dx[y], ten));
        const __m128i gy = _mm_sub_epi16(sx[y + 1], sx[y - 1]);
        xx = _mm_add_epi32(xx, _mm_madd_epi16(gx, gx)); yy = _mm_add_epi32(yy, _mm_madd_epi16(gy, gy)); xy = _mm_add_epi32(xy, _mm_madd_epi16(gx, gy));
    }
    return {Lanes(xx), Lanes(yy), Lanes(xy)};
}
// Two blocks a register: the first's row in the low 8 lanes, the second's in the high 8. Plain loops (no lambda: GCC gives a
// lambda in a target("avx2") function none of its target).
MEDIA_SURVEY_AVX2_TARGET
void TensorsAvx2(const std::uint8_t* p0, const std::uint8_t* p1, unsigned stride, Tensor& t0, Tensor& t1) noexcept {
    const __m256i three = _mm256_set1_epi16(3), ten = _mm256_set1_epi16(10);
    __m256i dx[10], sx[10];
    for (unsigned j = 0; j < 10; ++j) {
        const std::size_t at = static_cast<std::size_t>(j) * stride;
        __m256i v[3];
        for (unsigned k = 0; k < 3; ++k)
            v[k] = _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p0 + at + k)),
                                                           _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p1 + at + k))));
        dx[j] = _mm256_sub_epi16(v[2], v[0]);
        sx[j] = _mm256_add_epi16(_mm256_mullo_epi16(_mm256_add_epi16(v[0], v[2]), three), _mm256_mullo_epi16(v[1], ten));
    }
    __m256i xx = _mm256_setzero_si256(), yy = xx, xy = xx;
    for (unsigned y = 1; y < 9; ++y) {
        const __m256i gx = _mm256_add_epi16(_mm256_mullo_epi16(_mm256_add_epi16(dx[y - 1], dx[y + 1]), three), _mm256_mullo_epi16(dx[y], ten));
        const __m256i gy = _mm256_sub_epi16(sx[y + 1], sx[y - 1]);
        xx = _mm256_add_epi32(xx, _mm256_madd_epi16(gx, gx)); yy = _mm256_add_epi32(yy, _mm256_madd_epi16(gy, gy));
        xy = _mm256_add_epi32(xy, _mm256_madd_epi16(gx, gy));
    }
    t0 = {Lanes(_mm256_castsi256_si128(xx)), Lanes(_mm256_castsi256_si128(yy)), Lanes(_mm256_castsi256_si128(xy))};
    t1 = {Lanes(_mm256_extracti128_si256(xx, 1)), Lanes(_mm256_extracti128_si256(yy, 1)), Lanes(_mm256_extracti128_si256(xy, 1))};
}
#endif
// The largest code that is at most `level` as the shader compares (code / 255 <= level, in float): 0..255, or -1 for none.
int CodeAtMost(float level) noexcept {
    int code = -1;
    for (int c = 0; c < 256; ++c) if (static_cast<float>(c) / 255.0f <= level) code = c;
    return code;
}
// The block's line (its tensor `t`, its window at `p`) into the words, as the shader decides: strong (energy), straight
// (coherence), not a bar's edge, within the histograms' range. `dark`: a bar's codes are at most this.
void AddBlock(const Tensor& t, const std::uint8_t* p, unsigned stride, unsigned step, int dark, std::uint32_t* words) noexcept {
    const std::int64_t energy = std::int64_t{t.xx} + t.yy, d = std::int64_t{t.xx} - t.yy;
    if (static_cast<double>(energy) < static_cast<double>(kEnergy) * kUnit) return;
    // coherence^2 = (d^2 + 4 xy^2) / energy^2 (no root: the weight is its square).
    const double e = static_cast<double>(energy), coherence2 = (static_cast<double>(d) * static_cast<double>(d) + 4.0 * static_cast<double>(t.xy) * static_cast<double>(t.xy)) / (e * e);
    if (coherence2 < static_cast<double>(kCoherence) * static_cast<double>(kCoherence)) return;
    float angle = static_cast<float>(0.5 * std::atan2(2.0 * static_cast<double>(t.xy), static_cast<double>(d)) * 57.29577951308232);
    unsigned lines = 1;
    if (std::fabs(angle) > 45) { angle -= angle > 0 ? 90.0f : -90.0f; lines = 0; }
    if (std::fabs(angle) < HorizonLevel::kBarAxis) {
        // The edge of a bar encoded in the picture: a whole end row (column) of the window black.
        bool first = true, last = true;
        for (unsigned i = 0; i < 10; ++i) {
            const std::size_t a = lines ? static_cast<std::size_t>(i) * stride : static_cast<std::size_t>(i) * step;
            const std::size_t b = lines ? static_cast<std::size_t>(i) * stride + 9u * step : 9u * static_cast<std::size_t>(stride) + static_cast<std::size_t>(i) * step;
            first = first && p[a] <= dark; last = last && p[b] <= dark;
        }
        if (first || last) return;
    }
    const float at = (angle + 16.0f) * 2.0f;
    if (at < 0 || at >= static_cast<float>(HorizonLevel::kBins)) return;
    const unsigned bin = (std::min)(static_cast<unsigned>(at), HorizonLevel::kBins - 1), weight = static_cast<unsigned>(16.0 * coherence2 + 0.5);
    const unsigned fraction = (std::min)(static_cast<unsigned>((at - static_cast<float>(bin)) * 256.0f), 255u);
    words[2 * (lines * HorizonLevel::kBins + bin)] += weight; words[2 * (lines * HorizonLevel::kBins + bin) + 1] += weight * fraction;
}
// The peak of one histogram (each bin's weight and its blocks' mean place in it, a fraction of the bin): the five bins with the
// most weight against the median bin (the background); the tilt the mean shift of the weight above the background within a
// degree (two bins) of the last mean, from the peak's bin until it settles. A fixed five-bin window off the line's centre read
// the tilt up to half a degree toward the window's middle. False without a peak (less than `least` over the background, or
// not twice the background's share in its five bins).
bool Peak(const float* total, const float* place, float least, float& tilt, float& mass) noexcept {
    constexpr unsigned kBins = HorizonLevel::kBins;
    float sorted[kBins];
    for (unsigned b = 0; b < kBins; ++b) sorted[b] = total[b];
    std::nth_element(sorted, sorted + kBins / 2, sorted + kBins);
    const float background = sorted[kBins / 2];
    unsigned peak = 0; float best = -1;
    for (unsigned b = 0; b < kBins; ++b) {
        float sum = 0;
        for (int k = -2; k <= 2; ++k) { const int i = static_cast<int>(b) + k; if (i >= 0 && i < static_cast<int>(kBins)) sum += total[i]; }
        if (sum > best) { best = sum; peak = b; }
    }
    mass = best - 5.0f * background;
    if (mass < least || mass < 5.0f * background) return false;
    float at = static_cast<float>(peak) + 0.5f;
    for (unsigned step = 0; step < 8; ++step) {
        double weight = 0, sum = 0;
        for (unsigned b = 0; b < kBins; ++b) {
            if (std::fabs(place[b] - at) > 2.0f || total[b] <= background) continue;
            weight += total[b] - background; sum += (static_cast<double>(total[b]) - background) * place[b];
        }
        if (!(weight > 0)) return false;
        const float next = static_cast<float>(sum / weight);
        const bool settled = std::fabs(next - at) < 0.01f;
        at = next;
        if (settled) break;
    }
    tilt = HorizonLevel::kLowest + at * HorizonLevel::kBinDegrees;
    return true;
}
}
HorizonLevel::Measurement HorizonLevel::Measure(const std::uint32_t* words) noexcept {
    Measurement m;
    if (!words) return m;
    // Both classes in one histogram (the vertical lines at kVerticalWeight), and each on its own.
    float total[kBins], place[kBins], own[2][kBins], own_place[2][kBins];
    for (unsigned b = 0; b < kBins; ++b) {
        for (unsigned k = 0; k < 2; ++k) {
            const float w = static_cast<float>(words[2 * (k * kBins + b)]), within = static_cast<float>(words[2 * (k * kBins + b) + 1]) / 256.0f;
            own[k][b] = w; own_place[k][b] = static_cast<float>(b) + (w > 0 ? within / w : 0.5f);
        }
        total[b] = own[0][b] + kVerticalWeight * own[1][b];
        const float within = (static_cast<float>(words[2 * b + 1]) + kVerticalWeight * static_cast<float>(words[2 * (kBins + b) + 1])) / 256.0f;
        place[b] = static_cast<float>(b) + (total[b] > 0 ? within / total[b] : 0.5f);
    }
    for (unsigned k = 0; k < 2; ++k) { float mass = 0; if (Peak(own[k], own_place[k], kMinMass, m.lines[k], mass)) m.classes |= 1u << k; }
    float tilt = 0;
    if (!Peak(total, place, kMinMass, tilt, m.mass)) return m;
    // Lines in perspective: a roll turns the horizontal and the vertical lines alike; vanishing lines do not agree.
    if (m.classes == 3 && std::fabs(m.lines[0] - m.lines[1]) > kDisagree) { m.perspective = true; return m; }
    if (std::fabs(tilt) > kMaxTilt) return m;
    m.found = true; m.tilt = tilt;
    return m;
}
void HorizonLevel::Accumulate(const std::uint8_t* luma, unsigned stride, unsigned width, unsigned height, std::uint32_t* words, unsigned step,
                              unsigned black) noexcept {
    if (!luma || !words || width < 16 || height < 16 || !step) return;
    const int dark = CodeAtMost(static_cast<float>(black) / 255.0f + kLift); // A bar's samples are at most this (HdrToneMap.hlsl kMatteLift).
    // The shader's blocks: s texels a side (about kAcross across, a multiple of 8), the Scharr gradients at the 8 x 8 texels at
    // each one's centre, the texels from `origin` (the block's centre minus 5) to origin + 9. The blocks whose window fits in
    // the picture: columns [first, last) of every row whose window fits.
    const unsigned s = (std::max)(8u, (width / kAcross + 4) / 8 * 8);
    unsigned first = 0, last = width / s;
    while (first < last && first * s + s / 2 < 5) ++first;
    while (last > first && (last - 1) * s + s / 2 + 4 > width - 1) --last;
    [[maybe_unused]] const unsigned level = step == 1 ? detail::g_survey_level : 0u; // The vector loads read adjacent texels.
    for (unsigned by = 0; by < height / s; ++by) {
        const int oy = static_cast<int>(by * s + s / 2) - 5;
        if (oy < 0 || oy + 9 > static_cast<int>(height) - 1) continue;
        const std::uint8_t* row = luma + static_cast<std::size_t>(oy) * stride;
        const auto window = [&](unsigned bx) { return row + static_cast<std::size_t>(bx * s + s / 2 - 5) * step; };
        unsigned bx = first;
#if MEDIA_SURVEY_SSE2
        if (level >= 2)
            for (; bx + 2 <= last; bx += 2) {
                Tensor t0, t1; TensorsAvx2(window(bx), window(bx + 1), stride, t0, t1);
                AddBlock(t0, window(bx), stride, step, dark, words); AddBlock(t1, window(bx + 1), stride, step, dark, words);
            }
        if (level >= 1)
            for (; bx < last; ++bx) AddBlock(TensorSse2(window(bx), stride), window(bx), stride, step, dark, words);
#endif
        for (; bx < last; ++bx) AddBlock(TensorScalar(window(bx), stride, step), window(bx), stride, step, dark, words);
    }
}
unsigned HorizonLevel::ForceLevel(unsigned level) noexcept {
    detail::g_survey_level = (std::min)(level, detail::SurveyAvailable());
    return detail::g_survey_level;
}
unsigned HorizonLevel::Level() noexcept { return detail::g_survey_level; }
void HorizonLevel::Reset() noexcept {
    count_ = next_ = measured_ = pushed_ = 0; last_pts_ = INT64_MIN; locked_ = false; tilt_ = target_ = 0;
}
void HorizonLevel::Push(std::int64_t pts, const Measurement& m) noexcept {
    ++pushed_;
    // Presentation time since the newest picture pushed (pictures arrive in decode order: one presented earlier adds none).
    const float dt = last_pts_ == INT64_MIN || pts <= last_pts_ ? 0.0f : static_cast<float>(static_cast<double>(pts - last_pts_) / 1e7);
    if (last_pts_ == INT64_MIN || pts > last_pts_) last_pts_ = pts;
    if (m.found) {
        ++measured_;
        tilts_[next_] = m.tilt; next_ = (next_ + 1) % kWindow; count_ = (std::min)(count_ + 1, kWindow);
        float sorted[kWindow];
        for (unsigned i = 0; i < count_; ++i) sorted[i] = tilts_[i];
        std::nth_element(sorted, sorted + count_ / 2, sorted + count_);
        const float median = sorted[count_ / 2];
        unsigned agree = 0;
        for (unsigned i = 0; i < count_; ++i) agree += std::fabs(tilts_[i] - median) <= 1.0f ? 1u : 0u;
        if (agree >= kLock) { target_ = median; locked_ = true; }
    }
    // The level shown glides to the target (a quarter of a second's time constant), from where it was.
    if (locked_) tilt_ += (target_ - tilt_) * (1.0f - std::exp(-(std::min)(dt, 0.5f) / kGlide));
}
}
