// GPU live ingest (docs/sdk.md, "GPU live ingest"; docs/picture.md, "Stabilisation"), through the C SDK (API 4) on Windows:
// camera frames pushed with media_engine_ingest_video_frame into a sensor opened with MEDIA_SENSOR_GPU go through the
// production GPU passes (LiveGpuPipeline: the upload ring, ColorPipeline, NeuralUpscalePass) and come back as RGBA images.
// - shadow lift: a luma ramp's dark half rises, from Y' 0.75 up nothing moves, the ramp stays monotonic;
// - micro-motion: a 16 x 16 patch pulsing +-2 codes comes out pulsing several times as much, the flat surround unchanged;
// - stabilisation: a textured scene shaken in yaw and pitch (+-3 and 2.5 degrees at 7 and 9 Hz) with the IMU's deltas: the
//   images' content moves a fraction of what the camera's pictures do; perception reports the flow with the camera's rotation
//   taken out (no looming; at most one salient tile a picture on average, of 256, with nothing moving in the scene);
// - 2x super-resolution: neural and edge-directed images of the same frame, flagged as such, alike on average;
// - micro-motion under shake (2026-10-27): the shaken textured scene again, with a still flat target in it pulsing +-2 codes;
//   stabilised, its pulse comes out at least 4x as large with micro-motion at 5x (the reference follows the moving view: every
//   image after the first flagged MEDIA_GPU_MICRO_FOLLOWED), the content still held to a fraction of the shake; a second still
//   target, a ramp of 1 code per pixel without a pulse, varies no more than without micro-motion (the reference is registered:
//   kept in place instead, the image's residual motion over the ramp came out as a 5x pulse);
// - ten-bit (MEDIA_SENSOR_GPU_10BIT): a P010 ramp one 10-bit code per column through shadow lift 1.0: rising on (nearly) every
//   column, no rise over 1 code, where the 8-bit path rises every fourth column by 3-4 codes;
// - roll: a horizon, the camera rolling +-5 degrees at 7 Hz with the IMU's deltas: stabilised, the horizon stays level within
//   1.5 camera px RMS (its rows across the image about their mean), several times better than unstabilised;
// - level and crop (API 6, 2026-10-30): a horizon tilted +4.5 degrees in a picture letterboxed 30 rows each side, with
//   auto-crop and auto-leveling: the image's horizon level within 0.35 degree and its top and bottom rows picture, where with
//   auto-crop alone it reads +4.5;
// - 0 allocations while ingesting and reading images (8-bit and P010, rolling).
#include "media/c_api.h"
#include "allocation_probe.hpp"
#include <windows.h>
#include <malloc.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
bool skipped = false; // No hardware Direct3D 12 device: CTest skip code.
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr unsigned W = 320, H = 240;
constexpr double kPi = 3.14159265358979323846;
std::int64_t Pts(unsigned f) { return 1000000 + static_cast<std::int64_t>(f) * 333333; }
std::uint32_t Hash(std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
double Noise(double u, double v) {
    const double fu = std::floor(u), fv = std::floor(v), a = u - fu, b = v - fv;
    const auto at = [](int i, int j) { return static_cast<double>(Hash(static_cast<std::uint32_t>(i) * 7919u + static_cast<std::uint32_t>(j) * 104729u + 5u) % 181u); };
    const int i = static_cast<int>(fu), j = static_cast<int>(fv);
    return (1 - a) * (1 - b) * at(i, j) + a * (1 - b) * at(i + 1, j) + (1 - a) * b * at(i, j + 1) + a * b * at(i + 1, j + 1);
}
struct Frame { std::uint8_t y[W * H]; std::uint8_t uv[W * H / 2]; };
struct Rig {
    void* storage = nullptr; media_engine_t* engine = nullptr; std::uint32_t frames = 0; std::uint32_t converted = 0;
    bool Open(unsigned out_w, unsigned out_h, std::uint32_t options) {
        media_engine_config_t config; media_engine_config_init(&config);
        config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0;
        storage = _aligned_malloc(media_engine_storage_bytes(), media_engine_storage_alignment());
        if (!storage || media_engine_create(&config, storage, media_engine_storage_bytes(), &engine) != MEDIA_OK) return false;
        media_sensor_config_t s; media_sensor_config_init(&s);
        s.width = W; s.height = H; s.options = options; s.output_width = out_w; s.output_height = out_h;
        const auto status = media_engine_open_sensor(engine, &s);
        if (status == MEDIA_ERROR_UNSUPPORTED) { std::puts("GPU live ingest: no hardware Direct3D 12 device, skipped"); skipped = true; return false; }
        return status == MEDIA_OK;
    }
    // One frame; then waits for its image (the test's own pacing: a real host polls).
    bool Push(const Frame& f, double yaw = 0, double pitch = 0, bool imu = false, media_gpu_image_t* image = nullptr, double roll = 0) {
        media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = W; frame.height = H;
        frame.planes[0] = f.y; frame.planes[1] = f.uv; frame.strides[0] = frame.strides[1] = W;
        if (imu) { frame.flags = MEDIA_SENSOR_IMU; frame.imu_yaw = static_cast<float>(yaw); frame.imu_pitch = static_cast<float>(pitch); frame.imu_roll = static_cast<float>(roll); }
        return Send(frame, image);
    }
    // A P010 frame (16-bit samples, 10 bits in the high bits).
    bool PushP010(const std::uint16_t* luma, const std::uint16_t* cbcr, media_gpu_image_t* image = nullptr, double roll = 0, bool imu = false) {
        media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_P010; frame.width = W; frame.height = H;
        frame.planes[0] = reinterpret_cast<const std::uint8_t*>(luma); frame.planes[1] = reinterpret_cast<const std::uint8_t*>(cbcr);
        frame.strides[0] = frame.strides[1] = 2 * W;
        if (imu) { frame.flags = MEDIA_SENSOR_IMU; frame.imu_roll = static_cast<float>(roll); }
        return Send(frame, image);
    }
    bool Send(media_sensor_frame_t& frame, media_gpu_image_t* image) {
        frame.timestamp_100ns = Pts(frames++);
        if (media_engine_ingest_video_frame(engine, &frame) != MEDIA_OK) return false;
        ++converted;
        media_gpu_image_t local{}; auto& out = image ? *image : local; out.struct_size = sizeof(out);
        for (const auto start = GetTickCount64(); GetTickCount64() - start < 3000;) {
            if (media_query_gpu_image(engine, &out) == MEDIA_OK && out.sequence >= converted) return true;
            SwitchToThread();
        }
        return false;
    }
    ~Rig() { if (engine) media_engine_destroy(engine); if (storage) _aligned_free(storage); }
};
double Luma(const media_gpu_image_t& im, unsigned x, unsigned y) {
    const auto* p = im.rgba + static_cast<std::size_t>(y) * im.stride + 4 * x;
    return (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]);
}
std::vector<double> LumaCopy(const media_gpu_image_t& im) {
    std::vector<double> out(static_cast<std::size_t>(im.width) * im.height);
    for (unsigned y = 0; y < im.height; ++y) for (unsigned x = 0; x < im.width; ++x) out[static_cast<std::size_t>(y) * im.width + x] = Luma(im, x, y);
    return out;
}
void Flat(Frame& f, std::uint8_t y) { std::memset(f.y, y, sizeof(f.y)); std::memset(f.uv, 128, sizeof(f.uv)); }

void TestShadowLift() {
    Rig r; if (!r.Open(W, H, MEDIA_SENSOR_GPU)) { CHECK(skipped); return; }
    static Frame ramp; Flat(ramp, 0);
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) ramp.y[y * W + x] = static_cast<std::uint8_t>(16 + x * 219 / (W - 1));
    media_picture_controls_t c; media_picture_controls_init(&c);
    media_gpu_image_t plain{}, lifted{};
    CHECK(media_engine_set_picture_controls(r.engine, &c) == MEDIA_OK && r.Push(ramp, 0, 0, false, &plain));
    const auto before = LumaCopy(plain);
    c.shadow_lift = 0.6f;
    CHECK(media_engine_set_picture_controls(r.engine, &c) == MEDIA_OK && r.Push(ramp, 0, 0, false, &lifted));
    const auto after = LumaCopy(lifted);
    unsigned rose = 0, dark = 0, bright = 0, moved_bright = 0, reversals = 0; double rise_010 = 0;
    for (unsigned x = 2; x + 2 < W; ++x) {
        const double y_in = (16 + x * 219.0 / (W - 1) - 16) / 219, a = before[(H / 2) * W + x], b = after[(H / 2) * W + x];
        if (y_in > 0.03 && y_in < 0.5) { ++dark; if (b > a + 0.5) ++rose; }
        if (y_in >= 0.76) { ++bright; if (std::fabs(b - a) > 1.0) ++moved_bright; }
        if (x > 2 && after[(H / 2) * W + x] + 0.75 < after[(H / 2) * W + x - 1]) ++reversals;
        if (std::fabs(y_in - 0.10) < 0.0025) rise_010 = (b - a) / 255;
    }
    std::printf("Shadow lift 0.6 on the GPU (flags %#x, %.3f ms): %u of %u dark columns rose (Y' 0.10 by %+.3f), %u of %u bright columns moved, %u reversals\n",
                lifted.flags, static_cast<double>(lifted.gpu_ms), rose, dark, rise_010, moved_bright, bright, reversals);
    CHECK((lifted.flags & MEDIA_GPU_GRADED) && !(plain.flags & MEDIA_GPU_GRADED) && rose == dark && dark > 100 && moved_bright == 0 && bright > 50 && reversals == 0);
    CHECK(rise_010 > 0.04);
}
void TestMicroMotion() {
    Rig r; if (!r.Open(W, H, MEDIA_SENSOR_GPU)) { CHECK(skipped); return; }
    media_picture_controls_t c; media_picture_controls_init(&c);
    static Frame f;
    // The patch's mean (output) per picture, and one surround pixel, for gain 1 and gain 5.
    double amplitude[2]{}, surround[2]{};
    for (unsigned run = 0; run < 2; ++run) {
        c.micro_motion = run ? 5.0f : 1.0f;
        CHECK(media_engine_set_picture_controls(r.engine, &c) == MEDIA_OK);
        double low = 1e9, high = -1e9;
        for (unsigned k = 0; k < 16; ++k) {
            Flat(f, 128);
            const auto code = static_cast<std::uint8_t>(k % 2 ? 130 : 126);
            for (unsigned y = 112; y < 128; ++y) for (unsigned x = 152; x < 168; ++x) f.y[y * W + x] = code;
            media_gpu_image_t im{};
            CHECK(r.Push(f, 0, 0, false, &im));
            if (k < 6) continue;
            double sum = 0; for (unsigned y = 114; y < 126; ++y) for (unsigned x = 154; x < 166; ++x) sum += Luma(im, x, y);
            sum /= 144; low = (std::min)(low, sum); high = (std::max)(high, sum);
            surround[run] = Luma(im, 40, 40);
            if (run && k == 15) CHECK(im.flags & MEDIA_GPU_MICRO_MOTION);
        }
        amplitude[run] = high - low;
    }
    std::printf("Micro-motion 5x on the GPU: a +-2-code pulse comes out %.2f codes peak to peak (%.2f without), %.1fx; the surround %.1f and %.1f\n",
                amplitude[1], amplitude[0], amplitude[1] / (std::max)(amplitude[0], 1e-6), surround[1], surround[0]);
    CHECK(amplitude[0] > 2 && amplitude[1] > 3 * amplitude[0] && std::fabs(surround[1] - surround[0]) <= 1.0);
}
// The best integer shift (in output pixels, within +-24) of `now`'s central window onto `ref` (SAD).
void Align(const std::vector<double>& ref, const std::vector<double>& now, unsigned w, unsigned h, int& bx, int& by) {
    double best = 1e300; bx = by = 0;
    for (int dy = -24; dy <= 24; ++dy)
        for (int dx = -24; dx <= 24; ++dx) {
            double sad = 0;
            for (unsigned y = h / 4; y < 3 * h / 4; y += 2)
                for (unsigned x = w / 4; x < 3 * w / 4; x += 2)
                    sad += std::fabs(now[y * w + x] - ref[static_cast<std::size_t>(static_cast<int>(y) + dy) * w + static_cast<unsigned>(static_cast<int>(x) + dx)]);
            if (sad < best) { best = sad; bx = dx; by = dy; }
        }
}
void TestStabilisation() {
    const double focal = W / 2.0; // 90 degrees across.
    static Frame f;
    double rms[2]{}; unsigned looming = 0, salient = 0, ego = 0, measured = 0; std::uint32_t flags = 0;
    for (unsigned run = 0; run < 2; ++run) {
        Rig r; if (!r.Open(W, H, MEDIA_SENSOR_GPU | (run ? MEDIA_SENSOR_STABILIZE : 0u))) { CHECK(skipped); return; }
        std::vector<double> reference; double sum = 0; unsigned n = 0; double yaw_before = 0, pitch_before = 0;
        for (unsigned k = 0; k < 50; ++k) {
            const double yaw = 3.0 * kPi / 180 * std::sin(2 * kPi * 7 * k / 30.0), pitch = 2.5 * kPi / 180 * std::sin(2 * kPi * 9 * k / 30.0 + 1);
            for (unsigned y = 0; y < H; ++y)
                for (unsigned x = 0; x < W; ++x) // The scene moved by (-f yaw, f pitch): this pixel shows the world point that was there.
                    f.y[y * W + x] = static_cast<std::uint8_t>(40 + Noise((x + focal * yaw) / 7.0, (y - focal * pitch) / 7.0));
            std::memset(f.uv, 128, sizeof(f.uv));
            media_gpu_image_t im{};
            CHECK(r.Push(f, yaw - yaw_before, pitch - pitch_before, k > 0, &im));
            yaw_before = yaw; pitch_before = pitch;
            if (run) {
                media_frame_telemetry_t t{}; t.struct_size = sizeof(t);
                if (k >= 8 && media_query_frame_telemetry(r.engine, &t) == MEDIA_OK) {
                    ++measured; looming += (t.flags & MEDIA_TELEMETRY_LOOMING) ? 1u : 0u; ego += (t.flags & MEDIA_TELEMETRY_EGO) ? 1u : 0u;
                    media_saliency_grid_t g{}; g.struct_size = sizeof(g);
                    if (media_query_saliency_grid(r.engine, &g) == MEDIA_OK) salient += g.salient_tiles;
                }
                flags = im.flags;
            }
            if (k < 10) continue; // The low-pass settles.
            const auto luma = LumaCopy(im);
            if (reference.empty()) { reference = luma; continue; }
            int dx = 0, dy = 0; Align(reference, luma, im.width, im.height, dx, dy);
            // In the camera's pixels: the stabilised image is the view's 1/1.25 region scaled to the output.
            const double scale = static_cast<double>(im.right - im.left) / im.width;
            sum += (dx * dx + dy * dy) * scale * scale; ++n;
        }
        rms[run] = std::sqrt(sum / (std::max)(n, 1u));
    }
    std::printf("Stabilisation on the GPU (+-3 / 2.5 degrees yaw / pitch at 7 / 9 Hz, the IMU's deltas): the images' content moves %.2f camera px RMS "
                "stabilised, %.2f px not (flags %#x); perception: %u of %u pictures with the camera's rotation taken out, looming %u, salient tiles %u\n",
                rms[1], rms[0], flags, ego, measured, looming, salient);
    CHECK(rms[0] > 4 && rms[1] < 0.35 * rms[0] && (flags & MEDIA_GPU_STABILIZED) && measured > 30 && ego == measured && looming == 0 && salient <= measured);
}
// The shaken scene of TestStabilisation, with a still flat target (60 x 60 at the world's centre) pulsing +-2 codes, and a
// still ramp (50 x 50, 1 code per pixel, 70 pixels to its left) that does not change.
void TestShakenMicroMotion() {
    const double focal = W / 2.0;
    static Frame f;
    double amplitude[2]{}, ramp[2]{}, rms = 0; unsigned followed = 0, measured = 0;
    for (unsigned run = 0; run < 2; ++run) {
        Rig r; if (!r.Open(W, H, MEDIA_SENSOR_GPU | MEDIA_SENSOR_STABILIZE)) { CHECK(skipped); return; }
        media_picture_controls_t c; media_picture_controls_init(&c); c.micro_motion = run ? 5.0f : 1.0f;
        CHECK(media_engine_set_picture_controls(r.engine, &c) == MEDIA_OK);
        double yaw_before = 0, pitch_before = 0, low = 1e9, high = -1e9, ramp_low = 1e9, ramp_high = -1e9; std::vector<double> reference; double sum = 0; unsigned n = 0;
        for (unsigned k = 0; k < 50; ++k) {
            const double yaw = 3.0 * kPi / 180 * std::sin(2 * kPi * 7 * k / 30.0), pitch = 2.5 * kPi / 180 * std::sin(2 * kPi * 9 * k / 30.0 + 1);
            const int pulse = k % 2 ? 2 : -2;
            for (unsigned y = 0; y < H; ++y)
                for (unsigned x = 0; x < W; ++x) {
                    const double wx = x + focal * yaw, wy = y - focal * pitch; // The world point this pixel shows.
                    const double rx = wx - (W / 2.0 - 70);
                    f.y[y * W + x] = std::fabs(wx - W / 2.0) < 30 && std::fabs(wy - H / 2.0) < 30 ? static_cast<std::uint8_t>(128 + pulse)
                                   : std::fabs(rx) < 25 && std::fabs(wy - H / 2.0) < 25 ? static_cast<std::uint8_t>(std::lround(128 + rx))
                                                                                        : static_cast<std::uint8_t>(40 + Noise(wx / 7.0, wy / 7.0));
                }
            std::memset(f.uv, 128, sizeof(f.uv));
            media_gpu_image_t im{};
            CHECK(r.Push(f, yaw - yaw_before, pitch - pitch_before, k > 0, &im));
            yaw_before = yaw; pitch_before = pitch;
            if (run && k > 0) { ++measured; followed += (im.flags & MEDIA_GPU_MICRO_FOLLOWED) ? 1u : 0u; }
            if (k < 10) continue; // The low-pass settles; the reference follows.
            double mean = 0; for (unsigned y = H / 2 - 6; y < H / 2 + 6; ++y) for (unsigned x = W / 2 - 6; x < W / 2 + 6; ++x) mean += Luma(im, x, y);
            mean /= 144; low = (std::min)(low, mean); high = (std::max)(high, mean);
            // The ramp, 70 px left of the centre in the world: 87.5 output px at the view's 1.25x.
            const unsigned ramp_x = W / 2 - 88;
            double on_ramp = 0; for (unsigned y = H / 2 - 4; y < H / 2 + 4; ++y) for (unsigned x = ramp_x - 4; x < ramp_x + 4; ++x) on_ramp += Luma(im, x, y);
            on_ramp /= 64; ramp_low = (std::min)(ramp_low, on_ramp); ramp_high = (std::max)(ramp_high, on_ramp);
            if (run) {
                // The content still held: the image's shift against the first (the target's edges and the texture around it).
                const auto luma = LumaCopy(im);
                if (reference.empty()) reference = luma;
                else {
                    int dx = 0, dy = 0; Align(reference, luma, im.width, im.height, dx, dy);
                    const double scale = static_cast<double>(im.right - im.left) / im.width;
                    sum += (dx * dx + dy * dy) * scale * scale; ++n;
                }
            }
        }
        amplitude[run] = high - low; ramp[run] = ramp_high - ramp_low;
        if (run) rms = std::sqrt(sum / (std::max)(n, 1u));
    }
    std::printf("Micro-motion 5x under shake (stabilised, +-3 / 2.5 degrees at 7 / 9 Hz): the still target's +-2-code pulse comes out %.2f codes peak to peak "
                "(%.2f without), %.1fx; the still ramp varies %.2f codes (%.2f without); the reference followed the view on %u of %u images; the content moves %.2f camera px RMS\n",
                amplitude[1], amplitude[0], amplitude[1] / (std::max)(amplitude[0], 1e-6), ramp[1], ramp[0], followed, measured, rms);
    CHECK(amplitude[0] > 2 && amplitude[1] >= 4 * amplitude[0] && followed == measured && measured > 40 && rms < 3.0);
    CHECK(ramp[1] <= 1.5 * ramp[0] + 1.0);
}
// A P010 ramp, one 10-bit code per column from black (64) up, through shadow lift 1.0: ten-bit and 8-bit GPU paths.
void TestTenBit() {
    static std::uint16_t luma[W * H], cbcr[W * H / 2];
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) luma[y * W + x] = static_cast<std::uint16_t>((64 + x) << 6);
    for (auto& v : cbcr) v = static_cast<std::uint16_t>(512 << 6);
    constexpr unsigned kColumns = 160; // Codes 64..223: the shadows, where the lift is steepest.
    unsigned rises[2]{}; double steepest[2]{}; std::uint32_t flags[2]{};
    for (unsigned run = 0; run < 2; ++run) {
        Rig r; if (!r.Open(W, H, MEDIA_SENSOR_GPU | (run ? MEDIA_SENSOR_GPU_10BIT : 0u))) { CHECK(skipped); return; }
        media_picture_controls_t c; media_picture_controls_init(&c); c.shadow_lift = 1.0f;
        media_gpu_image_t im{};
        CHECK(media_engine_set_picture_controls(r.engine, &c) == MEDIA_OK && r.PushP010(luma, cbcr, &im));
        flags[run] = im.flags;
        // Each column's luma averaged over every row (the dither averages out).
        double before = 0;
        for (unsigned x = 0; x < kColumns; ++x) {
            double mean = 0; for (unsigned y = 0; y < H; ++y) mean += Luma(im, x, y);
            mean /= H;
            if (x) { const double step = mean - before; rises[run] += step > 0.1 ? 1u : 0u; steepest[run] = (std::max)(steepest[run], step); }
            before = mean;
        }
    }
    std::printf("Ten-bit P010 on the GPU (a ramp of one 10-bit code per column, shadow lift 1.0): 10-bit path (flags %#x) rises on %u of %u columns, at most %.2f codes; "
                "8-bit path (flags %#x) on %u, by up to %.2f codes\n", flags[1], rises[1], kColumns - 1, steepest[1], flags[0], rises[0], steepest[0]);
    CHECK((flags[1] & MEDIA_GPU_TEN_BIT) && !(flags[0] & MEDIA_GPU_TEN_BIT) && (flags[1] & MEDIA_GPU_GRADED));
    CHECK(rises[1] >= (kColumns - 1) * 9 / 10 && steepest[1] <= 1.0 && rises[0] <= (kColumns - 1) * 35 / 100 && steepest[0] >= 1.5);
}
// A horizon (bright sky over dark ground, through the centre), the camera rolling +-5 degrees at 7 Hz.
void TestRoll() {
    static Frame f;
    double rms[2]{}; float roll_seen = 0;
    for (unsigned run = 0; run < 2; ++run) {
        Rig r; if (!r.Open(W, H, MEDIA_SENSOR_GPU | (run ? MEDIA_SENSOR_STABILIZE : 0u))) { CHECK(skipped); return; }
        double before = 0, sum = 0; unsigned n = 0;
        for (unsigned k = 0; k < 50; ++k) {
            const double roll = 5.0 * kPi / 180 * std::sin(2 * kPi * 7 * k / 30.0);
            // The camera rolled clockwise by `roll` turns the scene by -roll on the picture: pixel r shows the world point R(roll) r.
            for (unsigned y = 0; y < H; ++y)
                for (unsigned x = 0; x < W; ++x) {
                    const double rx = x + 0.5 - W / 2.0, ry = y + 0.5 - H / 2.0, wy = std::sin(roll) * rx + std::cos(roll) * ry;
                    const double t = std::clamp(wy + 0.5, 0.0, 1.0); // Anti-aliased over a pixel.
                    f.y[y * W + x] = static_cast<std::uint8_t>(std::lround(180 - 120 * t));
                }
            std::memset(f.uv, 128, sizeof(f.uv));
            media_gpu_image_t im{};
            CHECK(r.Push(f, 0, 0, k > 0, &im, roll - before));
            before = roll;
            if (run) roll_seen = (std::max)(roll_seen, std::fabs(im.roll));
            if (k < 10) continue;
            // Where the horizon crosses each column (luma 120, between rows), about this image's mean row; in camera pixels.
            const double scale = static_cast<double>(im.right - im.left) / im.width;
            double rows[W]; unsigned m = 0; double mean = 0;
            for (unsigned x = im.width / 8; x < im.width * 7 / 8; ++x)
                for (unsigned y = 1; y < im.height; ++y) {
                    const double a = Luma(im, x, y - 1), b = Luma(im, x, y);
                    if (a >= 120 && b < 120) { rows[m] = y - 1 + (a - 120) / (a - b); mean += rows[m]; ++m; break; }
                }
            if (!m) continue;
            mean /= m;
            for (unsigned i = 0; i < m; ++i) { sum += (rows[i] - mean) * (rows[i] - mean) * scale * scale; ++n; }
        }
        rms[run] = std::sqrt(sum / (std::max)(n, 1u));
    }
    std::printf("Roll on the GPU (+-5 degrees at 7 Hz, the IMU's deltas): the horizon's rows across the image %.2f camera px RMS from level stabilised "
                "(the view turned up to %.2f degrees), %.2f px not\n", rms[1], roll_seen * 180 / kPi, rms[0]);
    CHECK(rms[1] <= 1.5 && rms[0] > 3 * rms[1] && roll_seen > 0.06f);
}
void TestSuperResolution() {
    Rig r; if (!r.Open(2 * W, 2 * H, MEDIA_SENSOR_GPU)) { CHECK(skipped); return; }
    static Frame f; Flat(f, 128);
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) f.y[y * W + x] = static_cast<std::uint8_t>(30 + Noise(x / 3.0, y / 3.0));
    media_picture_controls_t c; media_picture_controls_init(&c);
    double mean[2]{}; std::uint32_t flags[2]{}; float gpu[2]{};
    for (unsigned k = 0; k < 2; ++k) {
        c.reconstruction = k ? MEDIA_RECONSTRUCT_NEURAL : MEDIA_RECONSTRUCT_EDGE;
        media_gpu_image_t im{};
        CHECK(media_engine_set_picture_controls(r.engine, &c) == MEDIA_OK && r.Push(f, 0, 0, false, &im));
        CHECK(im.width == 2 * W && im.height == 2 * H);
        double sum = 0; for (unsigned y = 0; y < im.height; y += 3) for (unsigned x = 0; x < im.width; x += 3) sum += Luma(im, x, y);
        mean[k] = sum / ((im.height + 2) / 3) / ((im.width + 2) / 3); flags[k] = im.flags; gpu[k] = im.gpu_ms;
    }
    std::printf("2x on the GPU: edge-directed (flags %#x, %.3f ms) mean %.2f, neural (flags %#x, %.3f ms) mean %.2f\n", flags[0], static_cast<double>(gpu[0]), mean[0],
                flags[1], static_cast<double>(gpu[1]), mean[1]);
    CHECK((flags[0] & MEDIA_GPU_EDGE) && (flags[1] & MEDIA_GPU_NEURAL) && std::fabs(mean[0] - mean[1]) < 2.0 && gpu[0] > 0 && gpu[1] > 0);
}
void TestAllocations() {
    Rig r; if (!r.Open(2 * W, 2 * H, MEDIA_SENSOR_GPU | MEDIA_SENSOR_STABILIZE)) { CHECK(skipped); return; }
    media_picture_controls_t c; media_picture_controls_init(&c); c.shadow_lift = 0.4f; c.micro_motion = 3; c.reconstruction = MEDIA_RECONSTRUCT_NEURAL;
    static Frame f; Flat(f, 90);
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) f.y[y * W + x] = static_cast<std::uint8_t>(30 + Noise(x / 5.0, y / 5.0));
    CHECK(media_engine_set_picture_controls(r.engine, &c) == MEDIA_OK && r.Push(f, 0, 0, true));
    std::uint32_t pushed = 0;
    allocation_probe::Begin();
    for (unsigned k = 0; k < 40; ++k) pushed += r.Push(f, 0.001 * (k % 3), -0.001 * (k % 2), true) ? 1u : 0u;
    const auto allocations = allocation_probe::End();
    std::printf("Steady state: %u frames through the GPU (neural, lift, micro-motion, stabilised), %llu allocations\n", pushed, static_cast<unsigned long long>(allocations));
    CHECK(pushed == 40 && allocations == 0);
    // Ten-bit P010 frames, the camera rolling (the view turned, micro-motion following it).
    Rig t; if (!t.Open(2 * W, 2 * H, MEDIA_SENSOR_GPU | MEDIA_SENSOR_STABILIZE | MEDIA_SENSOR_GPU_10BIT)) { CHECK(skipped); return; }
    static std::uint16_t luma[W * H], cbcr[W * H / 2];
    for (unsigned i = 0; i < W * H; ++i) luma[i] = static_cast<std::uint16_t>((4 * f.y[i]) << 6);
    for (auto& v : cbcr) v = static_cast<std::uint16_t>(512 << 6);
    CHECK(media_engine_set_picture_controls(t.engine, &c) == MEDIA_OK && t.PushP010(luma, cbcr, nullptr, 0, true));
    std::uint32_t pushed10 = 0; media_gpu_image_t last{};
    allocation_probe::Begin();
    for (unsigned k = 0; k < 40; ++k) pushed10 += t.PushP010(luma, cbcr, &last, 0.01 * ((k % 4) - 1.5), true) ? 1u : 0u;
    const auto allocations10 = allocation_probe::End();
    std::printf("Steady state, ten-bit: %u P010 frames (rolling; flags %#x), %llu allocations\n", pushed10, last.flags, static_cast<unsigned long long>(allocations10));
    CHECK(pushed10 == 40 && allocations10 == 0 && (last.flags & MEDIA_GPU_TEN_BIT) && (last.flags & MEDIA_GPU_MICRO_FOLLOWED));
}
}

void TestLevelCrop() {
    static Frame f;
    constexpr unsigned kBars = 30;
    // A horizon (sky 180 over ground 60), the picture turned clockwise by 4.5 degrees (its right end lower), and furrows in the
    // ground for texture; then the letterbox.
    const double tilt = 4.5 * kPi / 180;
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) {
            const double rx = x + 0.5 - W / 2.0, ry = y + 0.5 - H / 2.0, wy = -std::sin(tilt) * rx + std::cos(tilt) * ry;
            const double t = std::clamp(wy + 0.5, 0.0, 1.0);
            f.y[y * W + x] = static_cast<std::uint8_t>(y < kBars || y >= H - kBars ? 16 : std::lround(180 - 120 * t + (wy > 8 && std::fmod(wy, 16.0) < 3 ? -20 : 0)));
        }
    std::memset(f.uv, 128, sizeof(f.uv));
    double measured[2]{}; double edges[2]{255, 255};
    for (unsigned run = 0; run < 2; ++run) {
        Rig r; if (!r.Open(W, H, MEDIA_SENSOR_GPU)) { CHECK(skipped); return; }
        CHECK(media_engine_set_auto_crop(r.engine, 1) == MEDIA_OK);
        if (run) CHECK(media_engine_set_auto_level(r.engine, 1) == MEDIA_OK);
        media_gpu_image_t im{};
        for (unsigned k = 0; k < 45; ++k) CHECK(r.Push(f, 0, 0, false, &im));
        // Where the horizon crosses each column (luma 120): the line through them, in degrees (+: right end lower).
        double sx = 0, sy = 0, sxx = 0, sxy = 0; unsigned n = 0;
        for (unsigned x = im.width / 8; x < im.width * 7 / 8; ++x)
            for (unsigned y = 1; y < im.height; ++y) {
                const double a = Luma(im, x, y - 1), b = Luma(im, x, y);
                if (a >= 120 && b < 120) { const double row = y - 1 + (a - 120) / (a - b); sx += x; sy += row; sxx += 1.0 * x * x; sxy += x * row; ++n; break; }
            }
        measured[run] = n > 2 ? std::atan((n * sxy - sx * sy) / (n * sxx - sx * sx)) * 180 / kPi : 99;
        for (unsigned x = 0; x < im.width; ++x) edges[run] = (std::min)({edges[run], Luma(im, x, 0), Luma(im, x, im.height - 1)});
    }
    std::printf("Level and crop on the GPU (+4.5 degrees, letterboxed %u rows): auto-crop alone, the image's horizon at %+.2f degrees, its edge rows at least %.0f; "
                "with auto-leveling %+.2f, at least %.0f\n", kBars, measured[0], edges[0], measured[1], edges[1]);
    CHECK(std::fabs(measured[0] - 4.5) <= 0.35 && std::fabs(measured[1]) <= 0.35 && edges[0] > 12 && edges[1] > 12); // A bar: 0; the furrows: 28.
}
int main() {
    std::puts("GPU live ingest tests");
    TestShadowLift();
    if (skipped) return 77;
    TestMicroMotion();
    TestStabilisation();
    TestShakenMicroMotion();
    TestTenBit();
    TestRoll();
    TestSuperResolution();
    TestLevelCrop();
    TestAllocations();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "GPU live ingest tests passed", failures);
    return failures ? 1 : 0;
}
