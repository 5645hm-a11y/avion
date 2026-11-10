#include "media/video/TensorExport.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::video {
std::size_t TensorBytes(const TensorSpec& s) noexcept {
    if (!s.width || !s.height || s.width > 8192 || s.height > 8192 || (s.channels != 1 && s.channels != 3)) return 0;
    if (s.format != TensorSpec::kFloat32 && s.format != TensorSpec::kFloat16) return 0;
    if (s.normalize > TensorSpec::kSigned) return 0;
    return std::size_t{s.width} * s.height * s.channels * (s.format == TensorSpec::kFloat32 ? 4u : 2u);
}
std::uint16_t HalfFromFloat(float value) noexcept {
    std::uint32_t x; std::memcpy(&x, &value, 4);
    const auto sign = static_cast<std::uint16_t>((x >> 16) & 0x8000u);
    const std::uint32_t magnitude = x & 0x7FFFFFFFu;
    if (magnitude > 0x7F800000u) return static_cast<std::uint16_t>(sign | 0x7E00u); // NaN.
    const int exponent = static_cast<int>(magnitude >> 23) - 127 + 15;
    std::uint32_t mantissa = magnitude & 0x7FFFFFu;
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);            // Too large: infinity.
    if (exponent <= 0) {
        // Subnormal: the value is (2^23 + mantissa) 2^(e - 150), the half's unit 2^-24.
        if (exponent < -10) return sign;
        mantissa |= 0x800000u;
        const unsigned shift = static_cast<unsigned>(14 - exponent);
        std::uint32_t h = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (h & 1u))) ++h;
        return static_cast<std::uint16_t>(sign | h);
    }
    std::uint32_t h = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    const std::uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) ++h; // A carry into the exponent is the right rounding (up to infinity).
    return static_cast<std::uint16_t>(sign | h);
}
namespace {
constexpr double kM1 = 0.1593017578125, kM2 = 78.84375, kC1 = 0.8359375, kC2 = 18.8515625, kC3 = 18.6875;
constexpr double kTargetNits = 203; // SDR white: HDR reference white.
double HlgToScene(double e) noexcept {
    constexpr double a = 0.17883277, b = 0.28466892, c = 0.55991073;
    return e <= 0.5 ? e * e / 3 : (std::exp((e - c) / a) + b) / 12;
}
// BT.2390 with black at 0: [0, source] onto [0, target], in PQ.
double Eetf(double pq, double source, double target) noexcept {
    const double e1 = pq / source, most = target / source;
    if (most >= 1) return pq;
    const double ks = 1.5 * most - 0.5;
    if (e1 < ks) return e1 * source;
    const double t = (e1 - ks) / (1 - ks), t2 = t * t, t3 = t2 * t;
    const double e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * most;
    return (std::min)(e2, most) * source;
}
}
double PqToNits(double e) noexcept {
    const double p = std::pow((std::max)(e, 0.0), 1 / kM2);
    return 10000 * std::pow((std::max)((p - kC1) / (kC2 - kC3 * p), 0.0), 1 / kM1);
}
double NitsToPq(double nits) noexcept {
    const double p = std::pow((std::max)(nits, 0.0) / 10000, kM1);
    return std::pow((std::max)((kC1 + kC2 * p) / (1 + kC3 * p), 0.0), kM2);
}
void HdrToDisplay(const TensorColor& color, double y, double cb, double cr, double rgb[3]) noexcept {
    // Y'CbCr, BT.2020 non-constant luminance.
    const double span = color.ten_bit ? 1023.0 / 4 : 255.0;
    const double luma = color.full_range ? y / span : (y - 16) / 219, pb = (cb - 128) / (color.full_range ? span : 224), pr = (cr - 128) / (color.full_range ? span : 224);
    constexpr double kr = 0.2627, kb = 0.0593;
    const double red = luma + 2 * (1 - kr) * pr, blue = luma + 2 * (1 - kb) * pb, green = (luma - kr * red - kb * blue) / (1 - kr - kb);
    const double e[3]{std::clamp(red, 0.0, 1.0), std::clamp(green, 0.0, 1.0), std::clamp(blue, 0.0, 1.0)};
    double nits[3];
    if (color.transfer == 18) {
        // HLG: scene light, then the BT.2100 OOTF for a 1000 cd/m2 display.
        const double s[3]{HlgToScene(e[0]), HlgToScene(e[1]), HlgToScene(e[2])};
        const double gain = 1000 * std::pow((std::max)(kr * s[0] + 0.6780 * s[1] + kb * s[2], 1e-6), 0.2);
        for (unsigned c = 0; c < 3; ++c) nits[c] = gain * s[c];
    } else for (unsigned c = 0; c < 3; ++c) nits[c] = PqToNits(e[c]);
    const double brightest = (std::max)({nits[0], nits[1], nits[2]});
    const double mapped = PqToNits(Eetf(NitsToPq(brightest), (std::max)(NitsToPq(color.peak_nits), 1e-4), NitsToPq(kTargetNits)));
    const double scale = brightest > 0 ? mapped / brightest / kTargetNits : 0;
    const double l[3]{nits[0] * scale, nits[1] * scale, nits[2] * scale};
    // BT.2020 to BT.709 (ITU-R BT.2087); colours out of its gamut desaturate toward their luminance.
    double out[3]{1.660491 * l[0] - 0.587641 * l[1] - 0.072850 * l[2], -0.124550 * l[0] + 1.132900 * l[1] - 0.008349 * l[2], -0.018151 * l[0] - 0.100579 * l[1] + 1.118730 * l[2]};
    const double lum = 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2], lowest = (std::min)({out[0], out[1], out[2]});
    if (lowest < 0 && lum > 0) for (double& v : out) v = lum + (v - lum) * (lum / (lum - lowest));
    for (unsigned c = 0; c < 3; ++c) rgb[c] = std::pow(std::clamp(out[c], 0.0, 1.0), 1 / 2.4);
}
namespace {
// One element from its cell's Y', Cb and Cr means (8-bit codes): BT.709 limited range to R'G'B' (an HDR picture: as shown,
// TensorColor), normalised, into its planes.
void Element(const TensorSpec& spec, void* out, std::size_t at, double y, double cb, double cr, const TensorColor& color) noexcept {
    const std::size_t plane = std::size_t{spec.width} * spec.height;
    const auto put = [&](unsigned c, double v) {
        double n = v;
        if (spec.normalize == TensorSpec::kImageNet) n = spec.channels == 1 ? (v - kGrayMean) / kGrayStd : (v - kImageNetMean[c]) / kImageNetStd[c];
        else if (spec.normalize == TensorSpec::kSigned) n = 2 * v - 1;
        const unsigned slot = spec.channels == 3 && spec.bgr ? 2 - c : c; // c: 0 R, 1 G, 2 B.
        const auto value = static_cast<float>(n);
        if (spec.format == TensorSpec::kFloat32) static_cast<float*>(out)[slot * plane + at] = value;
        else static_cast<std::uint16_t*>(out)[slot * plane + at] = HalfFromFloat(value);
    };
    if (IsHdrTensor(color)) {
        double rgb[3]; HdrToDisplay(color, y, cb, cr, rgb);
        if (spec.channels == 1) put(0, 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]);
        else for (unsigned c = 0; c < 3; ++c) put(c, rgb[c]);
        return;
    }
    const double luma = (y - 16) / 219;
    if (spec.channels == 1) { put(0, std::clamp(luma, 0.0, 1.0)); return; }
    const double pb = (cb - 128) / 224, pr = (cr - 128) / 224;
    // BT.709: R = Y' + 2 (1 - Kr) Pr, B = Y' + 2 (1 - Kb) Pb, G from Y' = Kr R + Kg G + Kb B.
    constexpr double kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
    const double r = luma + 2 * (1 - kr) * pr, b = luma + 2 * (1 - kb) * pb;
    const double g = luma - 2 * kb * (1 - kb) / kg * pb - 2 * kr * (1 - kr) / kg * pr;
    put(0, std::clamp(r, 0.0, 1.0)); put(1, std::clamp(g, 0.0, 1.0)); put(2, std::clamp(b, 0.0, 1.0));
}
}
bool ExportTensor(const Planes420& p, AreaRect region, const TensorSpec& spec, void* out, std::size_t bytes, const TensorColor& color) noexcept {
    const std::size_t need = TensorBytes(spec);
    const bool chroma = spec.channels == 3 || IsHdrTensor(color); // An HDR luma tensor is of the colours shown.
    if (!need || !out || bytes < need || !p.y || (chroma && (!p.cb || !p.cr))) return false;
    region.left = (std::max)(region.left, 0.0); region.top = (std::max)(region.top, 0.0);
    region.right = (std::min)(region.right, static_cast<double>(p.width)); region.bottom = (std::min)(region.bottom, static_cast<double>(p.height));
    if (!(region.right - region.left > 0) || !(region.bottom - region.top > 0)) return false;
    const unsigned w = spec.width, h = spec.height;
    const double cell_w = (region.right - region.left) / w, cell_h = (region.bottom - region.top) / h;
    const unsigned cw = (p.width + 1) / 2, ch = (p.height + 1) / 2;
    for (unsigned oy = 0; oy < h; ++oy)
        for (unsigned ox = 0; ox < w; ++ox) {
            const AreaRect cell{region.left + ox * cell_w, region.top + oy * cell_h, region.left + (ox + 1) * cell_w, region.top + (oy + 1) * cell_h};
            const AreaRect half{cell.left / 2, cell.top / 2, cell.right / 2, cell.bottom / 2};
            const double y = AreaMean(p.y, p.luma_stride, p.width, p.height, cell);
            const double cb = chroma ? AreaMean(p.cb, p.chroma_stride, cw, ch, half) : 128, cr = chroma ? AreaMean(p.cr, p.chroma_stride, cw, ch, half) : 128;
            Element(spec, out, std::size_t{oy} * w + ox, y, cb, cr, color);
        }
    return true;
}
bool ExportTensorMeans(const float* means, const TensorSpec& spec, void* out, std::size_t bytes, const TensorColor& color) noexcept {
    const std::size_t need = TensorBytes(spec);
    if (!need || !out || !means || bytes < need) return false;
    const std::size_t n = std::size_t{spec.width} * spec.height;
    for (std::size_t at = 0; at < n; ++at) Element(spec, out, at, means[3 * at], means[3 * at + 1], means[3 * at + 2], color);
    return true;
}
}
