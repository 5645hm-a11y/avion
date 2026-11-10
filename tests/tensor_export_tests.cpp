// Tensor export (docs/picture.md, "Tensor export"; docs/sdk.md, API 9), 2026-11-02. Suite #204.
// Portable (MSVC; GCC -O2 and ASan/UBSan):
// - binary16: every finite half survives the round trip through float exactly; every midpoint between two neighbours rounds
//   to the even one; values past the largest round to infinity, below half the smallest subnormal to zero, NaN stays NaN;
// - video::ExportTensor on a 64 x 48 picture of random codes: at its own size every element is the sample's BT.709 limited-
//   range conversion (R'G'B' clamped to [0, 1]) within 2e-7, each channel a contiguous plane (NCHW), B G R with the flag, Y'
//   alone with one channel; ImageNet's and the signed normalisation; float16 the float32 tensor rounded once more (bit for
//   bit); at half size each element the 2 x 2 mean; a region with fractional edges against an area mean computed here pixel
//   by pixel (1e-6); a bad spec, a region off the picture or a short buffer refused; no allocation.
// The SDK in host mode with a 320 x 240 camera: before a picture MEDIA_ERROR_EMPTY; then the whole picture exactly as the class
// makes it from the frame's planes; "zoom in": the view's region (80, 60)-(240, 180); the fovea of a 224 x 224 tensor square,
// half the view high, at its centre; a custom region as asked; float16 at 1 x 3 x 224 x 224 is 301,056 bytes
// (media_tensor_bytes), MEDIA_ERROR_STORAGE below; tracking a textured square that moves right, the fovea follows it (its
// centre within 6 samples); no allocation in the exports.
// 2026-11-03 (API 10; Windows): the windowed engine on the two-targets fixture (hidden, 3 s): a tensor of the whole picture
// (64 x 48, float32) and of the fovea (32 x 32, float16, ImageNet) while it plays, from the GPU, within 2e-3 of what host mode
// (its software bridge decoding the same I_PCM pictures) exports for the same picture (info.pts), each in at most 100 ms;
// a request while paused before playing: MEDIA_ERROR_EMPTY after the second it waits; no allocation in the second export.
// 2026-11-04 (API 11): an HDR picture's elements as the screen shows them (video::HdrToDisplay): PQ's 58 cd/m2 (below the tone
// map's knee) 58 / 203 at gamma 2.4, not its coded 0.45; the source's peak at 1; black 0; a ramp monotonic; every colour in [0, 1];
// HLG's reference white (75 %) where PQ's 203 cd/m2 is; the same through video::ExportTensor and its means, luma R'G'B' weighted.
// Windows: paused, the whole picture from the window is the picture on screen (its pts holds, host mode's export of it within 2e-3,
// a snapshot of the screen within a few codes), the second export from the held source (MEDIA_TENSOR_HELD, nothing decoded), and a
// host that has exported before has its first export while paused held too; an HDR fixture's picture, tone-mapped
// (MEDIA_TENSOR_TONE_MAPPED), in [0, 1], and within a few codes of the screen, where its coded values are not.
#include "media/c_api.h"
#include "media/video/TensorExport.hpp"
#include "allocation_probe.hpp"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <fstream>
#include <thread>
#include <windows.h>
#include <shellapi.h>
#endif

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)

// binary16 to float, written out (the test's own decoder).
float FromHalf(std::uint16_t h) {
    const unsigned sign = h >> 15, exponent = (h >> 10) & 31u, mantissa = h & 1023u;
    double v;
    if (exponent == 0) v = std::ldexp(static_cast<double>(mantissa), -24);
    else if (exponent == 31) v = mantissa ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else v = std::ldexp(1024.0 + mantissa, static_cast<int>(exponent) - 25);
    return static_cast<float>(sign ? -v : v);
}
void TestHalf() {
    unsigned trips = 0, midpoints = 0, bad = 0;
    for (unsigned h = 0; h < 65536; ++h) {
        const auto x = static_cast<std::uint16_t>(h);
        const unsigned exponent = (h >> 10) & 31u;
        if (exponent == 31) continue;
        const float f = FromHalf(x);
        if (video::HalfFromFloat(f) != x && !(f == 0 && (h & 0x7FFF) == 0)) ++bad;
        ++trips;
        // The midpoint to the next larger magnitude (not past the largest finite): to the even one of the two.
        if ((h & 0x7FFF) < 0x7BFF) {
            const auto next = static_cast<std::uint16_t>(h + 1);
            const double mid = (static_cast<double>(FromHalf(x)) + FromHalf(next)) / 2;
            const auto want = (h & 1u) ? next : x;
            if (video::HalfFromFloat(static_cast<float>(mid)) != want) ++bad;
            ++midpoints;
        }
    }
    const bool specials = video::HalfFromFloat(70000.0f) == 0x7C00 && video::HalfFromFloat(-70000.0f) == 0xFC00 && video::HalfFromFloat(1e-9f) == 0 &&
                          (video::HalfFromFloat(std::numeric_limits<float>::quiet_NaN()) & 0x7C00) == 0x7C00 &&
                          (video::HalfFromFloat(std::numeric_limits<float>::quiet_NaN()) & 0x3FF) != 0 && video::HalfFromFloat(65504.0f) == 0x7BFF &&
                          video::HalfFromFloat(1.0f) == 0x3C00 && video::HalfFromFloat(-2.0f) == 0xC000;
    std::printf("binary16: %u round trips, %u midpoints, %u wrong; specials %s\n", trips, midpoints, bad, specials ? "right" : "WRONG");
    CHECK(bad == 0 && trips == 63488 && specials);
}

constexpr unsigned kW = 64, kH = 48;
struct Lcg { std::uint32_t s; std::uint8_t Next() { s = s * 1664525u + 1013904223u; return static_cast<std::uint8_t>(s >> 24); } };
struct Picture { std::vector<std::uint8_t> y, cb, cr; video::Planes420 Planes() const { return {y.data(), cb.data(), cr.data(), kW, kW / 2, kW, kH}; } };
Picture Random() {
    Picture p; Lcg rng{2024};
    p.y.resize(kW * kH); p.cb.resize(kW * kH / 4); p.cr.resize(kW * kH / 4);
    for (auto& v : p.y) v = rng.Next();
    for (auto& v : p.cb) v = rng.Next();
    for (auto& v : p.cr) v = rng.Next();
    return p;
}
// BT.709 limited range to R'G'B', clamped (the reference: the spec's formulas, in double).
void Rgb(double y, double cb, double cr, double out[3]) {
    const double kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
    const double l = (y - 16) / 219, pb = (cb - 128) / 224, pr = (cr - 128) / 224;
    out[0] = l + 2 * (1 - kr) * pr; out[2] = l + 2 * (1 - kb) * pb; out[1] = l - 2 * kb * (1 - kb) / kg * pb - 2 * kr * (1 - kr) / kg * pr;
    for (int c = 0; c < 3; ++c) out[c] = std::fmin(1.0, std::fmax(0.0, out[c]));
}
// An area mean, pixel by pixel: each sample's unit square's overlap with the rectangle.
double Area(const std::uint8_t* plane, unsigned stride, unsigned w, unsigned h, double x0, double y0, double x1, double y1) {
    double sum = 0, weight = 0;
    for (unsigned j = 0; j < h; ++j)
        for (unsigned i = 0; i < w; ++i) {
            const double ox = std::fmax(0.0, std::fmin(x1, i + 1.0) - std::fmax(x0, static_cast<double>(i)));
            const double oy = std::fmax(0.0, std::fmin(y1, j + 1.0) - std::fmax(y0, static_cast<double>(j)));
            sum += ox * oy * plane[j * stride + i]; weight += ox * oy;
        }
    return sum / weight;
}
void TestExport() {
    const auto p = Random();
    video::TensorSpec spec; spec.width = kW; spec.height = kH;
    const std::size_t plane = kW * kH;
    std::vector<float> rgb(3 * plane), bgr(3 * plane), gray(plane), net(3 * plane), sig(3 * plane), half_ref(3 * plane);
    std::vector<std::uint16_t> half(3 * plane);
    allocation_probe::Begin();
    CHECK(video::ExportTensor(p.Planes(), {0, 0, kW, kH}, spec, rgb.data(), rgb.size() * 4));
    spec.bgr = true; CHECK(video::ExportTensor(p.Planes(), {0, 0, kW, kH}, spec, bgr.data(), bgr.size() * 4)); spec.bgr = false;
    spec.normalize = video::TensorSpec::kImageNet; CHECK(video::ExportTensor(p.Planes(), {0, 0, kW, kH}, spec, net.data(), net.size() * 4));
    spec.normalize = video::TensorSpec::kSigned; CHECK(video::ExportTensor(p.Planes(), {0, 0, kW, kH}, spec, sig.data(), sig.size() * 4));
    spec.normalize = video::TensorSpec::kUnit; spec.format = video::TensorSpec::kFloat16;
    CHECK(video::ExportTensor(p.Planes(), {0, 0, kW, kH}, spec, half.data(), half.size() * 2));
    spec.format = video::TensorSpec::kFloat32; spec.channels = 1;
    CHECK(video::ExportTensor(p.Planes(), {0, 0, kW, kH}, spec, gray.data(), gray.size() * 4));
    const auto allocations = allocation_probe::End();
    double worst = 0, worst_net = 0, worst_sig = 0, worst_gray = 0; unsigned half_wrong = 0, bgr_wrong = 0;
    for (unsigned y = 0; y < kH; ++y)
        for (unsigned x = 0; x < kW; ++x) {
            const std::size_t at = y * kW + x;
            double want[3]; Rgb(p.y[at], p.cb[(y / 2) * (kW / 2) + x / 2], p.cr[(y / 2) * (kW / 2) + x / 2], want);
            for (unsigned c = 0; c < 3; ++c) {
                worst = std::fmax(worst, std::fabs(rgb[c * plane + at] - want[c]));
                worst_net = std::fmax(worst_net, std::fabs(net[c * plane + at] - (want[c] - video::kImageNetMean[c]) / video::kImageNetStd[c]));
                worst_sig = std::fmax(worst_sig, std::fabs(sig[c * plane + at] - (2 * want[c] - 1)));
                if (bgr[(2 - c) * plane + at] != rgb[c * plane + at]) ++bgr_wrong;
                if (half[c * plane + at] != video::HalfFromFloat(rgb[c * plane + at])) ++half_wrong;
            }
            worst_gray = std::fmax(worst_gray, std::fabs(gray[at] - std::fmin(1.0, std::fmax(0.0, (p.y[at] - 16) / 219.0))));
        }
    std::printf("64x48 at its size: worst %.2e (R'G'B'), %.2e (ImageNet), %.2e (signed), %.2e (Y'); B G R planes %u wrong, float16 %u wrong; %llu allocations\n",
                worst, worst_net, worst_sig, worst_gray, bgr_wrong, half_wrong, static_cast<unsigned long long>(allocations));
    CHECK(worst < 2e-7 && worst_net < 1e-6 && worst_sig < 4e-7 && worst_gray < 2e-7 && bgr_wrong == 0 && half_wrong == 0 && allocations == 0);
    // Half size: each element the 2 x 2 mean (each covers one chroma sample).
    spec.channels = 3; spec.width = kW / 2; spec.height = kH / 2;
    std::vector<float> small(3 * plane / 4);
    CHECK(video::ExportTensor(p.Planes(), {0, 0, kW, kH}, spec, small.data(), small.size() * 4));
    double worst_small = 0;
    for (unsigned y = 0; y < kH / 2; ++y)
        for (unsigned x = 0; x < kW / 2; ++x) {
            const double luma = (p.y[2 * y * kW + 2 * x] + p.y[2 * y * kW + 2 * x + 1] + p.y[(2 * y + 1) * kW + 2 * x] + p.y[(2 * y + 1) * kW + 2 * x + 1]) / 4.0;
            double want[3]; Rgb(luma, p.cb[y * (kW / 2) + x], p.cr[y * (kW / 2) + x], want);
            for (unsigned c = 0; c < 3; ++c) worst_small = std::fmax(worst_small, std::fabs(small[c * plane / 4 + y * (kW / 2) + x] - want[c]));
        }
    // A region with fractional edges, to 10 x 7.
    const video::AreaRect region{3.25, 5.5, 41.75, 30.125};
    spec.width = 10; spec.height = 7;
    std::vector<float> odd(3 * 70);
    CHECK(video::ExportTensor(p.Planes(), region, spec, odd.data(), odd.size() * 4));
    double worst_odd = 0;
    for (unsigned y = 0; y < 7; ++y)
        for (unsigned x = 0; x < 10; ++x) {
            const double cw = (region.right - region.left) / 10, ch = (region.bottom - region.top) / 7;
            const double x0 = region.left + x * cw, y0 = region.top + y * ch, x1 = x0 + cw, y1 = y0 + ch;
            double want[3];
            Rgb(Area(p.y.data(), kW, kW, kH, x0, y0, x1, y1), Area(p.cb.data(), kW / 2, kW / 2, kH / 2, x0 / 2, y0 / 2, x1 / 2, y1 / 2),
                Area(p.cr.data(), kW / 2, kW / 2, kH / 2, x0 / 2, y0 / 2, x1 / 2, y1 / 2), want);
            for (unsigned c = 0; c < 3; ++c) worst_odd = std::fmax(worst_odd, std::fabs(odd[c * 70 + y * 10 + x] - want[c]));
        }
    std::printf("Half size worst %.2e; fractional region to 10x7 worst %.2e\n", worst_small, worst_odd);
    CHECK(worst_small < 2e-7 && worst_odd < 1e-6);
    // Refusals.
    video::TensorSpec bad; bad.channels = 2;
    CHECK(!video::ExportTensor(p.Planes(), {0, 0, kW, kH}, bad, odd.data(), odd.size() * 4) && video::TensorBytes(bad) == 0);
    CHECK(!video::ExportTensor(p.Planes(), {kW + 1.0, 0, kW + 9.0, 8}, spec, odd.data(), odd.size() * 4));
    CHECK(!video::ExportTensor(p.Planes(), region, spec, odd.data(), odd.size() * 4 - 1));
}

// The SDK.
constexpr std::size_t kStorage = 48u << 20;
std::vector<unsigned char> g_storage;
void* Aligned(unsigned char* base) {
    const std::size_t a = media_engine_storage_alignment();
    return base + (a - reinterpret_cast<std::uintptr_t>(base) % a) % a;
}
constexpr unsigned kSW = 320, kSH = 240;
// A camera picture: a smooth background and a textured 48 x 48 square at `left`, its chroma a warm tint.
void Scene(unsigned left, std::vector<std::uint8_t>& nv12) {
    nv12.assign(kSW * kSH * 3 / 2, 128);
    for (unsigned y = 0; y < kSH; ++y)
        for (unsigned x = 0; x < kSW; ++x) {
            double v = 60 + 20 * std::sin(x / 23.0) * std::cos(y / 19.0);
            if (x >= left && x < left + 48 && y >= 96 && y < 144) v = 150 + 60 * (((x - left) / 6 + (y - 96) / 6) % 2) + ((x * 7 + y * 3) % 11);
            nv12[y * kSW + x] = static_cast<std::uint8_t>(std::lround(v));
        }
    for (unsigned y = 0; y < kSH / 2; ++y)
        for (unsigned x = 0; x < kSW / 2; ++x) { nv12[kSW * kSH + y * kSW + 2 * x] = static_cast<std::uint8_t>(110 + x % 7); nv12[kSW * kSH + y * kSW + 2 * x + 1] = static_cast<std::uint8_t>(150 - y % 5); }
}
void TestSdk() {
    g_storage.assign(kStorage, 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY;
    media_engine_t* e = nullptr;
    CHECK(media_engine_create(&config, Aligned(g_storage.data()), kStorage - 4096, &e) == MEDIA_OK);
    if (!e) return;
    media_sensor_config_t sensor; media_sensor_config_init(&sensor);
    sensor.width = kSW; sensor.height = kSH;
    CHECK(media_engine_open_sensor(e, &sensor) == MEDIA_OK);
    media_tensor_request_t r; media_tensor_request_init(&r);
    CHECK(r.width == 224 && r.height == 224 && r.channels == 3 && r.format == MEDIA_TENSOR_F32 && r.region == MEDIA_TENSOR_FOVEA);
    static float tensor[3 * kSW * kSH];
    media_tensor_info_t info{}; info.struct_size = sizeof(info);
    CHECK(media_export_foveated_tensor(e, &r, tensor, sizeof(tensor), &info) == MEDIA_ERROR_EMPTY);
    std::vector<std::uint8_t> nv12;
    Scene(136, nv12);
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = kSW; frame.height = kSH;
    frame.planes[0] = nv12.data(); frame.planes[1] = nv12.data() + kSW * kSH; frame.strides[0] = frame.strides[1] = kSW; frame.timestamp_100ns = 0;
    CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
    // The whole picture: as the class makes it from the frame's own planes (NV12's chroma de-interleaved).
    std::vector<std::uint8_t> cb(kSW * kSH / 4), cr(kSW * kSH / 4);
    for (unsigned i = 0; i < kSW * kSH / 4; ++i) { cb[i] = nv12[kSW * kSH + 2 * i]; cr[i] = nv12[kSW * kSH + 2 * i + 1]; }
    static float expected[3 * kSW * kSH];
    video::TensorSpec spec; spec.width = kSW; spec.height = kSH;
    CHECK(video::ExportTensor({nv12.data(), cb.data(), cr.data(), kSW, kSW / 2, kSW, kSH}, {0, 0, kSW, kSH}, spec, expected, sizeof(expected)));
    r.width = kSW; r.height = kSH; r.region = MEDIA_TENSOR_FULL;
    std::uint64_t allocations = 0;
    allocation_probe::Begin();
    CHECK(media_export_foveated_tensor(e, &r, tensor, sizeof(tensor), &info) == MEDIA_OK);
    allocations += allocation_probe::End();
    CHECK(std::memcmp(tensor, expected, sizeof(expected)) == 0 && info.bytes == sizeof(expected) && info.left == 0 && info.right == kSW && info.pts_100ns == 0);
    // The view after "zoom in" (2x at the centre).
    CHECK(media_engine_command(e, "zoom in") == MEDIA_OK);
    frame.timestamp_100ns = 333333; CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
    r.width = 160; r.height = 120; r.region = MEDIA_TENSOR_VIEW;
    allocation_probe::Begin();
    CHECK(media_export_foveated_tensor(e, &r, tensor, sizeof(tensor), &info) == MEDIA_OK);
    allocations += allocation_probe::End();
    std::printf("View after zoom in: [%.1f, %.1f]-[%.1f, %.1f]\n", info.left, info.top, info.right, info.bottom);
    CHECK(std::fabs(info.left - 80) < 0.01f && std::fabs(info.top - 60) < 0.01f && std::fabs(info.right - 240) < 0.01f && std::fabs(info.bottom - 180) < 0.01f);
    // The fovea of a square tensor: half the view high (60), as wide, at its centre.
    r.width = r.height = 224; r.region = MEDIA_TENSOR_FOVEA; r.format = MEDIA_TENSOR_F16;
    CHECK(media_tensor_bytes(&r) == 301056);
    CHECK(media_export_foveated_tensor(e, &r, tensor, 301055, &info) == MEDIA_ERROR_STORAGE);
    allocation_probe::Begin();
    CHECK(media_export_foveated_tensor(e, &r, tensor, 301056, &info) == MEDIA_OK);
    allocations += allocation_probe::End();
    std::printf("Fovea (224x224, float16): [%.1f, %.1f]-[%.1f, %.1f]\n", info.left, info.top, info.right, info.bottom);
    CHECK(std::fabs(info.right - info.left - 80) < 0.01f && std::fabs(info.bottom - info.top - 80) < 0.01f);
    CHECK(std::fabs((info.left + info.right) / 2 - 160) < 0.01f && std::fabs((info.top + info.bottom) / 2 - 120) < 0.01f && info.target_track_id == 0);
    r.region = MEDIA_TENSOR_REGION; r.left = 0.25f; r.top = 0.5f; r.right = 0.75f; r.bottom = 1.0f; r.format = MEDIA_TENSOR_F32;
    CHECK(media_export_foveated_tensor(e, &r, tensor, sizeof(tensor), &info) == MEDIA_OK && info.left == 80 && info.top == 120 && info.right == 240 && info.bottom == 240);
    r.right = 1.5f;
    CHECK(media_export_foveated_tensor(e, &r, tensor, sizeof(tensor), &info) == MEDIA_ERROR_INVALID_ARGUMENT);
    // Tracking: the square at the centre, then moving right 2 samples a picture; the fovea follows it.
    CHECK(media_engine_command(e, "track the target") == MEDIA_OK);
    r.region = MEDIA_TENSOR_FOVEA; r.width = r.height = 96;
    float centre = -1;
    for (unsigned f = 2; f < 32; ++f) {
        const unsigned left = 136 + 2 * (f - 2);
        Scene(left, nv12); frame.planes[0] = nv12.data(); frame.planes[1] = nv12.data() + kSW * kSH; frame.timestamp_100ns = 333333ll * f;
        CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
        allocation_probe::Begin();
        CHECK(media_export_foveated_tensor(e, &r, tensor, sizeof(tensor), &info) == MEDIA_OK);
        allocations += allocation_probe::End();
        centre = (info.left + info.right) / 2;
    }
    media_playback_state_t state{}; CHECK(media_engine_state(e, &state) == MEDIA_OK);
    std::printf("Tracking (state %d): the fovea's centre at x %.1f, the square's at %u; %llu allocations in the exports\n", state.tracking, centre, 136 + 2 * 29 + 24,
                static_cast<unsigned long long>(allocations));
    CHECK(state.tracking == 1 && std::fabs(centre - (136 + 2 * 29 + 24)) <= 6);
    CHECK(allocations == 0);
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
}
void TestHdr() {
    using video::HdrToDisplay; using video::NitsToPq; using video::TensorColor;
    TensorColor pq; pq.transfer = 16; pq.peak_nits = 1000;
    TensorColor hlg; hlg.transfer = 18; hlg.peak_nits = 1000;
    const auto grey = [](const TensorColor& c, double e) { double rgb[3]; HdrToDisplay(c, 16 + 219 * e, 128, 128, rgb); return rgb[1]; };
    const double code58 = NitsToPq(58), shown58 = grey(pq, code58), expected58 = std::pow(58.0 / 203, 1 / 2.4);
    const double black = grey(pq, 0), peak = grey(pq, NitsToPq(1000)), above = grey(pq, NitsToPq(4000));
    double last = -1; bool monotonic = true, bounded = true;
    for (unsigned i = 0; i <= 200; ++i) { const double v = grey(pq, i / 200.0); monotonic = monotonic && v >= last - 1e-9; last = v; }
    for (unsigned y = 16; y <= 235; y += 5)
        for (unsigned cb = 16; cb <= 240; cb += 8)
            for (unsigned cr = 16; cr <= 240; cr += 8) {
                double rgb[3]; HdrToDisplay(pq, y, cb, cr, rgb);
                for (double v : rgb) bounded = bounded && v >= 0 && v <= 1;
                HdrToDisplay(hlg, y, cb, cr, rgb);
                for (double v : rgb) bounded = bounded && v >= 0 && v <= 1;
            }
    const double hlg_white = grey(hlg, 0.75), pq_white = grey(pq, NitsToPq(203));
    std::printf("HDR: PQ 58 cd/m2 (coded %.3f) shown %.4f for %.4f; black %.4f, the peak %.4f, 4000 cd/m2 %.4f; ramp monotonic %d, every colour in [0, 1] %d; HLG 75 %% %.4f, PQ 203 cd/m2 %.4f\n",
                code58, shown58, expected58, black, peak, above, monotonic, bounded, hlg_white, pq_white);
    CHECK(std::fabs(shown58 - expected58) <= 1e-4 && std::fabs(shown58 - code58) > 0.1 && black == 0);
    CHECK(std::fabs(peak - 1) <= 1e-3 && above >= peak - 1e-6 && monotonic && bounded && std::fabs(hlg_white - pq_white) <= 2e-3);
    // Through the export: a 16 x 16 picture of one grey, R G B and Y'; the means' path the same; no allocation.
    constexpr unsigned kSide = 16;
    const auto code = static_cast<std::uint8_t>(std::lround(16 + 219 * NitsToPq(203)));
    std::vector<std::uint8_t> y(kSide * kSide, code), c(kSide * kSide / 4, 128);
    const video::Planes420 planes{y.data(), c.data(), c.data(), kSide, kSide / 2, kSide, kSide};
    video::TensorSpec spec; spec.width = spec.height = 4;
    float rgb[3 * 16], luma[16], means[3 * 16], via_means[3 * 16];
    for (unsigned i = 0; i < 16; ++i) { means[3 * i] = code; means[3 * i + 1] = means[3 * i + 2] = 128; }
    allocation_probe::Begin();
    const bool made = video::ExportTensor(planes, {0, 0, kSide, kSide}, spec, rgb, sizeof(rgb), pq);
    const bool from_means = video::ExportTensorMeans(means, spec, via_means, sizeof(via_means), pq);
    spec.channels = 1;
    const bool made_luma = video::ExportTensor(planes, {0, 0, kSide, kSide}, spec, luma, sizeof(luma), pq);
    const auto allocations = allocation_probe::End();
    double expected[3]; HdrToDisplay(pq, code, 128, 128, expected);
    double worst = 0;
    for (unsigned i = 0; i < 16; ++i) for (unsigned k = 0; k < 3; ++k) worst = std::fmax(worst, std::fmax(std::fabs(rgb[k * 16 + i] - expected[k]), std::fabs(via_means[k * 16 + i] - expected[k])));
    const double y_expected = 0.2126 * expected[0] + 0.7152 * expected[1] + 0.0722 * expected[2];
    std::printf("HDR export: a 4 x 4 tensor of a grey picture within %.7f of the sample's, Y' %.4f for %.4f; %llu allocations\n", worst, static_cast<double>(luma[5]), y_expected, static_cast<unsigned long long>(allocations));
    CHECK(made && from_means && made_luma && worst <= 1e-6 && std::fabs(luma[5] - y_expected) <= 1e-6 && allocations == 0);
}
#if defined(_WIN32)
// The arguments' paths as UTF-8 from the wide command line (argv is in the ANSI code page: "Développement" would not survive).
void FixtureArgument(char* fixture, std::size_t capacity, int index = 1) {
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide && count > index) (void)WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, fixture, static_cast<int>(capacity), nullptr, nullptr);
    if (wide) LocalFree(wide);
}
// The screen as a snapshot wrote it: the means of (R' + G' + B' weighted as luma) over `across` x `down` cells, 0 to 1.
bool ScreenCells(media_engine_t* e, const char* path, unsigned across, unsigned down, std::vector<double>& cells) {
    if (media_engine_save_snapshot(e, path) != MEDIA_OK) return false;
    media_snapshot_t s{}; s.struct_size = sizeof(s);
    for (unsigned i = 0; i < 300; ++i) { // A snapshot's files are written on a thread of their own.
        Sleep(10);
        if (media_query_snapshot(e, &s) == MEDIA_OK && std::strcmp(s.path, path) == 0) break;
    }
    if (std::strcmp(s.path, path) != 0 || s.status != MEDIA_OK) return false;
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    (void)std::remove(path);
    (void)std::remove((std::string(path, std::strlen(path) - 4) + ".json").c_str());
    if (file.size() < 54 || file[0] != 'B' || file[1] != 'M') return false;
    const auto word = [&](unsigned at) { return static_cast<std::uint32_t>(file[at]) | file[at + 1] << 8 | file[at + 2] << 16 | static_cast<std::uint32_t>(file[at + 3]) << 24; };
    const unsigned w = word(18); const auto stored = static_cast<std::int32_t>(word(22)); const unsigned h = static_cast<unsigned>(stored < 0 ? -stored : stored);
    const unsigned stride = (w * 3 + 3) / 4 * 4;
    if (file.size() < 54 + std::size_t{stride} * h) return false;
    cells.assign(std::size_t{across} * down, 0);
    std::vector<unsigned> counts(cells.size());
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const unsigned char* p = file.data() + 54 + std::size_t{stored < 0 ? y : h - 1 - y} * stride + std::size_t{x} * 3; // B G R.
            const std::size_t cell = std::size_t{y * down / h} * across + x * across / w;
            cells[cell] += (0.2126 * p[2] + 0.7152 * p[1] + 0.0722 * p[0]) / 255; ++counts[cell];
        }
    for (std::size_t i = 0; i < cells.size(); ++i) cells[i] /= counts[i] ? counts[i] : 1;
    return true;
}
// Host mode's tensor of the fixture's picture at `pts` (its software bridge decodes the I_PCM pictures), stepped to it.
bool HostTensor(const char* fixture, const media_tensor_request_t& r, std::int64_t pts, float* out, std::size_t bytes) {
    static std::vector<unsigned char> storage;
    storage.assign(kStorage, 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY;
    media_engine_t* e = nullptr;
    if (media_engine_create(&config, Aligned(storage.data()), kStorage - 4096, &e) != MEDIA_OK || !e) return false;
    bool found = false;
    if (media_engine_open(e, fixture) == MEDIA_OK) {
        media_tensor_info_t info{}; info.struct_size = sizeof(info);
        for (std::int64_t now = 0; now < 60000000 && !found; now += 20000) { // 2 ms steps over 6 s.
            if (media_engine_step(e, now) != MEDIA_OK) break;
            if (media_export_foveated_tensor(e, &r, out, bytes, &info) == MEDIA_OK && info.pts_100ns >= pts) found = info.pts_100ns == pts;
            if (info.pts_100ns > pts) break;
        }
    }
    (void)media_engine_close(e);
    media_engine_destroy(e);
    return found;
}
void TestWindow(const char* fixture) {
    if (!fixture || !*fixture) { std::puts("Window: no fixture given, skipped"); return; }
    static std::vector<unsigned char> storage;
    storage.assign(media_engine_storage_bytes() + media_engine_storage_alignment(), 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.visible = 0; config.maximum_seconds = 3; config.wake_policy = MEDIA_WAKE_DISABLED;
    media_engine_t* e = nullptr;
    const std::size_t a = media_engine_storage_alignment();
    CHECK(media_engine_create(&config, storage.data() + (a - reinterpret_cast<std::uintptr_t>(storage.data()) % a) % a, media_engine_storage_bytes(), &e) == MEDIA_OK);
    if (!e) return;
    CHECK(media_engine_open(e, fixture) == MEDIA_OK);
    // Before playing nothing is converted: the request waits its second, then MEDIA_ERROR_EMPTY.
    media_tensor_request_t whole; media_tensor_request_init(&whole);
    whole.width = 64; whole.height = 48; whole.region = MEDIA_TENSOR_FULL;
    static float window_full[3 * 64 * 48], host_full[3 * 64 * 48];
    media_tensor_info_t full{}; full.struct_size = sizeof(full);
    const auto before = media_export_foveated_tensor(e, &whole, window_full, sizeof(window_full), &full);
    std::puts("Window: the two-targets fixture, hidden, 3 s; tensors at 1.5 s");
    std::thread player([e] { (void)media_engine_run(e); });
    Sleep(1500);
    auto start = std::chrono::steady_clock::now();
    const auto status_full = media_export_foveated_tensor(e, &whole, window_full, sizeof(window_full), &full);
    const double full_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    media_tensor_request_t fovea; media_tensor_request_init(&fovea);
    fovea.width = fovea.height = 32; fovea.format = MEDIA_TENSOR_F16; fovea.normalize = MEDIA_TENSOR_IMAGENET;
    static std::uint16_t window_fovea[3 * 32 * 32], host_fovea[3 * 32 * 32];
    media_tensor_info_t foveal{}; foveal.struct_size = sizeof(foveal);
    start = std::chrono::steady_clock::now();
    allocation_probe::Begin();
    const auto status_fovea = media_export_foveated_tensor(e, &fovea, window_fovea, sizeof(window_fovea), &foveal);
    const auto allocations = allocation_probe::End();
    const double fovea_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // The view's stabilisation through the SDK (API 11; the test is media_playback_stabilize_test): on, and off again.
    CHECK(media_engine_set_stabilization(e, 1) == MEDIA_OK);
    Sleep(100);
    CHECK(media_engine_set_stabilization(e, 0) == MEDIA_OK);
    player.join();
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
    // The same pictures in host mode.
    const bool host_full_found = status_full == MEDIA_OK && HostTensor(fixture, whole, full.pts_100ns, host_full, sizeof(host_full));
    const bool host_fovea_found = status_fovea == MEDIA_OK && HostTensor(fixture, fovea, foveal.pts_100ns, reinterpret_cast<float*>(host_fovea), sizeof(host_fovea));
    double worst_full = 0, worst_fovea = 0;
    for (unsigned i = 0; i < 3 * 64 * 48; ++i) worst_full = std::fmax(worst_full, std::fabs(window_full[i] - host_full[i]));
    for (unsigned i = 0; i < 3 * 32 * 32; ++i) worst_fovea = std::fmax(worst_fovea, std::fabs(FromHalf(window_fovea[i]) - FromHalf(host_fovea[i])));
    std::printf("Window: before playing %d; whole picture %d at %.3f s in %.1f ms ([%.1f, %.1f]-[%.1f, %.1f]), host mode's %s, worst difference %.6f; fovea %d at %.3f s "
                "in %.1f ms ([%.1f, %.1f]-[%.1f, %.1f]), host mode's %s, worst difference %.5f; %llu allocations in the second export\n", before, status_full,
                full.pts_100ns / 1e7, full_ms, full.left, full.top, full.right, full.bottom, host_full_found ? "found" : "NOT FOUND", worst_full, status_fovea,
                foveal.pts_100ns / 1e7, fovea_ms, foveal.left, foveal.top, foveal.right, foveal.bottom, host_fovea_found ? "found" : "NOT FOUND", worst_fovea,
                static_cast<unsigned long long>(allocations));
    CHECK(before == MEDIA_ERROR_EMPTY && status_full == MEDIA_OK && status_fovea == MEDIA_OK && full.right == 320 && full.bottom == 240);
    CHECK(host_full_found && worst_full <= 2e-3 && host_fovea_found && worst_fovea <= 1e-2); // Float16 of ImageNet's scale: a step is ~4e-3.
    CHECK(full_ms <= 100 && fovea_ms <= 100 && allocations == 0);
}
// Paused: the first export comes from a landing or a held picture, the second from the held source; both are the picture on screen.
void TestPaused(const char* fixture) {
    if (!fixture || !*fixture) { std::puts("Paused: no fixture given, skipped"); return; }
    static std::vector<unsigned char> storage;
    storage.assign(media_engine_storage_bytes() + media_engine_storage_alignment(), 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.visible = 0; config.maximum_seconds = 5; config.wake_policy = MEDIA_WAKE_DISABLED;
    media_engine_t* e = nullptr;
    const std::size_t a = media_engine_storage_alignment();
    CHECK(media_engine_create(&config, storage.data() + (a - reinterpret_cast<std::uintptr_t>(storage.data()) % a) % a, media_engine_storage_bytes(), &e) == MEDIA_OK);
    if (!e) return;
    CHECK(media_engine_open(e, fixture) == MEDIA_OK);
    std::thread player([e] { (void)media_engine_run(e); });
    Sleep(1000);
    CHECK(media_engine_command(e, "pause") == MEDIA_OK);
    Sleep(300);
    media_tensor_request_t whole; media_tensor_request_init(&whole);
    whole.width = 64; whole.height = 48; whole.region = MEDIA_TENSOR_FULL;
    static float first[3 * 64 * 48], second[3 * 64 * 48], third[3 * 64 * 48], host[3 * 64 * 48];
    const auto timed = [&](float* out, media_tensor_info_t& info, double& ms) {
        info.struct_size = sizeof(info);
        const auto start = std::chrono::steady_clock::now();
        const auto status = media_export_foveated_tensor(e, &whole, out, 3 * 64 * 48 * sizeof(float), &info);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return status;
    };
    media_tensor_info_t one{}, two{}, three{}; double one_ms = 0, two_ms = 0, three_ms = 0;
    const auto status_one = timed(first, one, one_ms);
    allocation_probe::Begin();
    const auto status_two = timed(second, two, two_ms);
    const auto allocations = allocation_probe::End();
    // On screen: a snapshot of what the window shows.
    std::vector<double> screen; const bool shot = ScreenCells(e, "tensor_export_tests_paused.bmp", 8, 6, screen);
    // A later pause: this host has exported before, so the picture is held from the pause on.
    CHECK(media_engine_command(e, "resume") == MEDIA_OK);
    Sleep(400);
    CHECK(media_engine_command(e, "pause") == MEDIA_OK);
    Sleep(300);
    const auto status_three = timed(third, three, three_ms);
    media_engine_stop(e);
    player.join();
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
    double same = 0;
    for (unsigned i = 0; i < 3 * 64 * 48; ++i) same = std::fmax(same, std::fabs(first[i] - second[i]));
    const bool host_found = status_one == MEDIA_OK && HostTensor(fixture, whole, one.pts_100ns, host, sizeof(host));
    double worst_host = 0;
    for (unsigned i = 0; i < 3 * 64 * 48; ++i) worst_host = std::fmax(worst_host, std::fabs(first[i] - host[i]));
    // The tensor's luma, cell by cell, against the screen.
    double worst_screen = 0;
    for (unsigned cell = 0; shot && cell < 48; ++cell) {
        double sum = 0;
        const unsigned cx = cell % 8, cy = cell / 8;
        for (unsigned y = cy * 8; y < cy * 8 + 8; ++y)
            for (unsigned x = cx * 8; x < cx * 8 + 8; ++x) sum += 0.2126 * first[y * 64 + x] + 0.7152 * first[64 * 48 + y * 64 + x] + 0.0722 * first[2 * 64 * 48 + y * 64 + x];
        worst_screen = std::fmax(worst_screen, std::fabs(sum / 64 - screen[cell]));
    }
    std::printf("Paused: first export %d at %.3f s in %.1f ms (flags %u), second %d at %.3f s in %.1f ms (flags %u), identical within %.6f; %llu allocations; host mode's %s, worst difference %.6f; "
                "the screen %s, worst cell %.4f; after a later pause %d at %.3f s in %.1f ms (flags %u)\n", status_one, one.pts_100ns / 1e7, one_ms, one.flags, status_two, two.pts_100ns / 1e7,
                two_ms, two.flags, same, static_cast<unsigned long long>(allocations), host_found ? "found" : "NOT FOUND", worst_host, shot ? "shot" : "NOT SHOT", worst_screen, status_three,
                three.pts_100ns / 1e7, three_ms, three.flags);
    CHECK(status_one == MEDIA_OK && status_two == MEDIA_OK && status_three == MEDIA_OK && one.pts_100ns == two.pts_100ns && one.pts_100ns >= 5000000 && same == 0);
    CHECK((two.flags & MEDIA_TENSOR_HELD) && (three.flags & MEDIA_TENSOR_HELD) && three.pts_100ns > one.pts_100ns);
    CHECK(host_found && worst_host <= 2e-3 && shot && worst_screen <= 0.02 && allocations == 0 && two_ms <= 100 && three_ms <= 100);
}
// An HDR fixture's picture: the elements what the screen shows, in [0, 1].
void TestHdrWindow(const char* fixture) {
    if (!fixture || !*fixture || GetFileAttributesA(fixture) == INVALID_FILE_ATTRIBUTES) {
        wchar_t wide[1024]{}; (void)MultiByteToWideChar(CP_UTF8, 0, fixture ? fixture : "", -1, wide, 1024);
        if (!fixture || !*fixture || GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES) { std::puts("HDR window: no fixture (no 10-bit HEVC encoder?), skipped"); return; }
    }
    static std::vector<unsigned char> storage;
    storage.assign(media_engine_storage_bytes() + media_engine_storage_alignment(), 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.visible = 0; config.maximum_seconds = 4; config.wake_policy = MEDIA_WAKE_DISABLED;
    media_engine_t* e = nullptr;
    const std::size_t a = media_engine_storage_alignment();
    CHECK(media_engine_create(&config, storage.data() + (a - reinterpret_cast<std::uintptr_t>(storage.data()) % a) % a, media_engine_storage_bytes(), &e) == MEDIA_OK);
    if (!e) return;
    CHECK(media_engine_open(e, fixture) == MEDIA_OK);
    std::thread player([e] { (void)media_engine_run(e); });
    Sleep(1500);
    CHECK(media_engine_command(e, "pause") == MEDIA_OK);
    Sleep(400);
    media_tensor_request_t whole; media_tensor_request_init(&whole);
    whole.width = 48; whole.height = 32; whole.region = MEDIA_TENSOR_FULL;
    static float rgb[3 * 48 * 32];
    media_tensor_info_t info{}; info.struct_size = sizeof(info);
    const auto status = media_export_foveated_tensor(e, &whole, rgb, sizeof(rgb), &info);
    std::vector<double> screen; const bool shot = ScreenCells(e, "tensor_export_tests_hdr.bmp", 8, 8, screen);
    media_engine_stop(e);
    player.join();
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
    double lowest = 1e9, highest = -1e9, worst_screen = 0, mean = 0;
    for (float v : rgb) { lowest = std::fmin(lowest, v); highest = std::fmax(highest, v); mean += v; }
    mean /= 3 * 48 * 32;
    for (unsigned cell = 0; shot && cell < 64; ++cell) {
        double sum = 0;
        const unsigned cx = cell % 8, cy = cell / 8;
        for (unsigned y = cy * 4; y < cy * 4 + 4; ++y)
            for (unsigned x = cx * 6; x < cx * 6 + 6; ++x) sum += 0.2126 * rgb[y * 48 + x] + 0.7152 * rgb[48 * 32 + y * 48 + x] + 0.0722 * rgb[2 * 48 * 32 + y * 48 + x];
        worst_screen = std::fmax(worst_screen, std::fabs(sum / 24 - screen[cell]));
    }
    std::printf("HDR window: %d at %.3f s (flags %u), elements %.4f to %.4f, mean %.4f; the screen %s, worst cell %.4f\n", status, info.pts_100ns / 1e7, info.flags, lowest, highest, mean, shot ? "shot" : "NOT SHOT", worst_screen);
    CHECK(status == MEDIA_OK && (info.flags & MEDIA_TENSOR_TONE_MAPPED) && lowest >= 0 && highest <= 1 && highest > lowest && shot && worst_screen <= 0.03);
}
#endif
}

int main() {
    std::puts("Tensor export tests"); // stdout's buffer before the first probe (MSVC Debug counts the CRT's).
    TestHalf();
    TestExport();
    TestHdr();
    TestSdk();
#if defined(_WIN32)
    char fixture[1024]{}, hdr[1024]{};
    FixtureArgument(fixture, sizeof(fixture));
    FixtureArgument(hdr, sizeof(hdr), 2);
    TestWindow(fixture);
    TestPaused(fixture);
    TestHdrWindow(hdr);
#endif
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "tensor export tests passed", failures);
    return failures ? 1 : 0;
}
