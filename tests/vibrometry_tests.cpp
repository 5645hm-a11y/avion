// Remote visual vibrometry and vitals (docs/picture.md, "Remote vibrometry"; docs/sdk.md, API 9), 2026-11-02. Suite #201.
// Portable (MSVC; GCC -O2 and ASan/UBSan). Synthetic 320 x 240 pictures at 30 per second (10 s): a textured background and a
// textured skin-coloured patch under the view's fovea, lit from one side (0.8 code a sample across), +-1 code of noise in
// every sample, rounded to 8 bits; the camera's shake moves the whole scene (3 px at 2.3 Hz + 1.5 px at 5.1 Hz across, 2.5 px
// at 3.7 Hz down, sub-pixel).
// video::Vibrometry on the fovea (moved with the shake when compensated):
// - a pulse: the patch's luma +-1.5 codes (and Cr +-0.8) at 1.20 Hz: 72.0 BPM within 1.5 BPM, high confidence; without
//   shake, with shake (compensated), and with the pictures' times jittered by +-3 ms;
// - a flicker: +-1.5 codes at 8.0 Hz: 8.0 Hz within 0.15 Hz, with and without shake;
// - a mechanical vibration: the patch's texture moving +-0.3 sample at 8.0 Hz (its mean unchanged): 8.0 Hz on a shift channel;
// - the same shake measured without compensation: the fovea's mean follows the lighting under it (2.4 codes at 2.3 Hz), and
//   the pulse is lost (the reading is the shake's 138 BPM, not 72): what the compensation is for;
// - a still scene (no noise) and a noise scene (+-10 codes per sample per picture): confidence 0, no frequency;
// - no allocation while pushing; the cost per picture.
// The SDK in host mode with a camera (no GPU): the pulse scene with the shake reported by the IMU (yaw and pitch from the
// scene's motion): media_query_frame_telemetry says 72 BPM within 1.5 (MEDIA_TELEMETRY_VITALS), the region the fovea; API 8's
// struct (400 bytes) still accepted; a still scene says no vibration; no allocation in the ingests.
// 2026-11-03 (API 10; docs/picture.md, "Moving targets", "The virtual IMU"):
// - a moving target: an 80 x 80 textured skin patch pulsing at 1.20 Hz crosses the picture (12 samples a second across, a
//   slow wave down) under a spotlight (its light 1.0 to 1.3 times as it goes): vibrometry following its path
//   (video::TargetPath from positions with +-0.6 sample of tracker jitter) reads 72 BPM within 1.5 with no restart and the
//   whole 8.5 s window; the old policy (the region moved when the target had left a third of it) restarts every few seconds
//   and never holds 2.5 s;
// - the same through the SDK with tracking ("track" when the target is at the view's centre): 72 BPM within 1.5,
//   MEDIA_VIBROMETRY_FOLLOWING, a window of 8 s or more, the region on the target;
// - a shaking clip with no IMU deltas (MEDIA_SENSOR_IMU unset): the virtual IMU's motion within 0.1 sample rms of the true
//   shake (MEDIA_TELEMETRY_VIRTUAL_IMU), the stabilised view within 0.3 sample rms of the same engine fed the true IMU, the
//   pulse at 72 BPM within 1.5 (138 uncompensated, above), and the event camera, stabilised by it, firing at most 5 % of what
//   it fires unstabilised and no more than 1.2 times what it fires stabilised by the true IMU (the rest is this scene's point-
//   sampled edges: the IMU leaves them too); no allocation in the ingests.
#include "media/c_api.h"
#include "media/video/Stabilizer.hpp"
#include "media/video/Vibrometry.hpp"
#include "media/video/VirtualImu.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kW = 320, kH = 240, kFrames = 300;
constexpr double kFps = 30.0;

struct Lcg { std::uint32_t s; int Next(int span) { s = s * 1664525u + 1013904223u; return static_cast<int>((s >> 8) % static_cast<std::uint32_t>(2 * span + 1)) - span; } };
enum class Signal { Pulse, Flicker, Vibration, Still, Noise };
struct Scene {
    Signal signal = Signal::Pulse;
    double hz = 1.2;
    bool shake = false;
    bool noise = true;
};
// The camera's shake: how far the scene has moved on the picture at time t (pixels, +x right, +y down).
void Shake(double t, double& sx, double& sy) {
    sx = 3.0 * std::sin(2 * kPi * 2.3 * t) + 1.5 * std::sin(2 * kPi * 5.1 * t + 1.0);
    sy = 2.5 * std::sin(2 * kPi * 3.7 * t + 0.5);
}
struct Picture { std::vector<std::uint8_t> y, cb, cr; };
// The scene at time t: point samples of smooth functions (the background's waves, the patch's own texture) at the scene's
// coordinates (the picture's minus the shake), the signal, +-1 code of noise, rounded.
void Make(const Scene& s, double t, Lcg& rng, Picture& p) {
    p.y.resize(kW * kH); p.cb.resize(kW * kH / 4); p.cr.resize(kW * kH / 4);
    double sx = 0, sy = 0; if (s.shake) Shake(t, sx, sy);
    const double wave = std::sin(2 * kPi * s.hz * t);
    const double pulse = s.signal == Signal::Pulse || s.signal == Signal::Flicker ? 1.5 * wave : 0;
    const double move = s.signal == Signal::Vibration ? 0.3 * wave : 0; // The patch's content, samples across.
    const auto patch = [](double x, double y) { return x >= 50 && x < 270 && y >= 35 && y < 205; };
    for (unsigned j = 0; j < kH; ++j)
        for (unsigned i = 0; i < kW; ++i) {
            const double x = i + 0.5 - sx, y = j + 0.5 - sy;
            double v;
            if (s.signal == Signal::Still) v = 128;
            else if (patch(x, y)) {
                const double u = x - move;
                // Lit from the right (0.8 code a sample: shaken 3 samples, the fovea's mean moves 2.4 codes) and textured.
                v = 150 + 0.8 * (u - 160) + 14 * std::sin(2 * kPi * u / 23.0) * std::cos(2 * kPi * y / 31.0) + 6 * std::sin(2 * kPi * (u + y) / 13.0) + pulse;
            } else v = 100 + 40 * std::sin(2 * kPi * x / 37.0) * std::cos(2 * kPi * y / 29.0) + 15 * std::sin(2 * kPi * (x - 2 * y) / 17.0);
            if (s.signal == Signal::Noise) v = 128 + rng.Next(10);
            else if (s.noise) v += rng.Next(1);
            p.y[j * kW + i] = static_cast<std::uint8_t>(std::lround(std::fmin(255, std::fmax(0, v))));
        }
    for (unsigned j = 0; j < kH / 2; ++j)
        for (unsigned i = 0; i < kW / 2; ++i) {
            const double x = 2 * i + 1 - sx, y = 2 * j + 1 - sy;
            const bool skin = s.signal != Signal::Still && s.signal != Signal::Noise && patch(x, y);
            double cb = skin ? 112 : 128, cr = skin ? 148 + (s.signal == Signal::Pulse ? 0.8 * wave : 0) : 128;
            if (s.noise && s.signal != Signal::Still) { cb += rng.Next(1); cr += rng.Next(1); }
            p.cb[j * (kW / 2) + i] = static_cast<std::uint8_t>(std::lround(cb)); p.cr[j * (kW / 2) + i] = static_cast<std::uint8_t>(std::lround(cr));
        }
}
std::int64_t Pts(unsigned f, bool jitter) {
    const double t = f / kFps + (jitter ? 0.003 * std::sin(f * 2.1) : 0.0);
    return static_cast<std::int64_t>(std::llround(t * 1e7));
}
// The fovea of a 1x view: half the picture across and down at its centre, moved with the shake when compensated.
video::AreaRect Fovea(double t, bool compensate, bool shake) {
    double sx = 0, sy = 0; if (shake && compensate) Shake(t, sx, sy);
    return {80 + sx, 60 + sy, 240 + sx, 180 + sy};
}
struct Run { video::VibrometryResult result; double us_per_picture = 0; std::uint64_t allocations = 0; };
Run Measure(const Scene& s, bool compensate = true, bool jitter = false) {
    // Every picture made first (allocation here), then pushed (no allocation there).
    std::vector<Picture> pictures(kFrames);
    Lcg rng{12345};
    for (unsigned f = 0; f < kFrames; ++f) Make(s, Pts(f, jitter) / 1e7, rng, pictures[f]);
    static video::Vibrometry v; // ~25 KB: not on the stack.
    v.Reset();
    Run run;
    const auto start = std::chrono::steady_clock::now();
    allocation_probe::Begin();
    for (unsigned f = 0; f < kFrames; ++f) {
        const auto& p = pictures[f];
        const video::Planes420 planes{p.y.data(), p.cb.data(), p.cr.data(), kW, kW / 2, kW, kH};
        v.Push(planes, Fovea(Pts(f, jitter) / 1e7, compensate, s.shake), Pts(f, jitter));
    }
    run.allocations = allocation_probe::End();
    run.us_per_picture = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kFrames;
    run.result = v.Result();
    return run;
}
const char* ChannelName(std::uint32_t c) {
    static const char* const kNames[]{"luma", "Cb", "Cr", "shift x", "shift y"};
    return c < 5 ? kNames[c] : "?";
}
void Print(const char* name, const Run& r) {
    const auto& v = r.result;
    std::printf("%-34s vibration %7.3f Hz (conf %.2f, snr %8.1f, %s, amplitude %.3f), vital %6.2f BPM (conf %.2f, %s), %u pictures %.2f s, %.1f us/picture\n", name,
                v.vibration_hz, v.confidence, v.snr, ChannelName(v.channel), v.amplitude, v.vital_bpm, v.vital_confidence, ChannelName(v.vital_channel), v.samples,
                v.seconds, r.us_per_picture);
}

void TestClass() {
    {
        const auto r = Measure({Signal::Pulse, 1.2, false});
        Print("pulse 1.2 Hz", r);
        CHECK(r.result.known && std::fabs(r.result.vital_bpm - 72.0f) <= 1.5f && r.result.vital_confidence >= 0.8f);
        CHECK(std::fabs(r.result.vibration_hz - 1.2f) <= 0.025f && r.result.confidence >= 0.8f);
        CHECK(r.allocations == 0);
    }
    {
        const auto r = Measure({Signal::Pulse, 1.2, true});
        Print("pulse 1.2 Hz, shake compensated", r);
        CHECK(std::fabs(r.result.vital_bpm - 72.0f) <= 1.5f && r.result.vital_confidence >= 0.8f);
        const auto raw = Measure({Signal::Pulse, 1.2, true}, false);
        Print("pulse 1.2 Hz, shake not compensated", raw);
        CHECK(!(std::fabs(raw.result.vital_bpm - 72.0f) <= 1.5f));
    }
    {
        const auto r = Measure({Signal::Pulse, 1.2, true}, true, true);
        Print("pulse 1.2 Hz, shake, jittered times", r);
        CHECK(std::fabs(r.result.vital_bpm - 72.0f) <= 1.5f && r.result.vital_confidence >= 0.8f);
    }
    {
        const auto r = Measure({Signal::Flicker, 8.0, false});
        Print("flicker 8 Hz", r);
        CHECK(std::fabs(r.result.vibration_hz - 8.0f) <= 0.15f && r.result.confidence >= 0.8f);
        CHECK(r.result.vital_bpm < 0); // Its 4th harmonic folds to 2 Hz at 30/s: not a pulse.
        const auto shaken = Measure({Signal::Flicker, 8.0, true});
        Print("flicker 8 Hz, shake compensated", shaken);
        CHECK(std::fabs(shaken.result.vibration_hz - 8.0f) <= 0.15f && shaken.result.confidence >= 0.8f);
    }
    {
        const auto r = Measure({Signal::Vibration, 8.0, false});
        Print("vibration 8 Hz +-0.3 sample", r);
        CHECK(std::fabs(r.result.vibration_hz - 8.0f) <= 0.15f && r.result.confidence >= 0.8f && r.result.channel == video::VibrometryResult::kShiftX);
        CHECK(r.result.amplitude > 0.25f && r.result.amplitude < 0.35f);
        const auto shaken = Measure({Signal::Vibration, 8.0, true});
        Print("vibration 8 Hz, shake compensated", shaken);
        CHECK(std::fabs(shaken.result.vibration_hz - 8.0f) <= 0.15f && shaken.result.confidence >= 0.8f);
    }
    {
        const auto r = Measure({Signal::Still, 0, false, false});
        Print("still scene", r);
        CHECK(r.result.known && r.result.confidence == 0 && r.result.vital_confidence == 0 && r.result.vibration_hz < 0 && r.result.vital_bpm < 0);
        const auto n = Measure({Signal::Noise, 0, false});
        Print("noise scene", n);
        CHECK(n.result.known && n.result.confidence == 0 && n.result.vital_confidence == 0 && n.result.vibration_hz < 0);
        CHECK(n.result.snr < video::Vibrometry::kSnrFloor);
    }
}

// The SDK: a camera whose IMU reports the shake.
constexpr std::size_t kStorage = 48u << 20;
std::vector<unsigned char> g_storage;
void* Aligned(unsigned char* base) {
    const std::size_t a = media_engine_storage_alignment();
    return base + (a - reinterpret_cast<std::uintptr_t>(base) % a) % a;
}
void TestSdk(Signal signal) {
    const Scene s{signal, 1.2, true};
    std::vector<std::vector<std::uint8_t>> nv12(kFrames);
    Lcg rng{777};
    Picture p;
    for (unsigned f = 0; f < kFrames; ++f) {
        Make(s, Pts(f, false) / 1e7, rng, p);
        auto& out = nv12[f]; out.resize(kW * kH * 3 / 2);
        std::memcpy(out.data(), p.y.data(), kW * kH);
        for (unsigned i = 0; i < kW * kH / 4; ++i) { out[kW * kH + 2 * i] = p.cb[i]; out[kW * kH + 2 * i + 1] = p.cr[i]; }
    }
    g_storage.assign(kStorage, 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY;
    media_engine_t* e = nullptr;
    CHECK(media_engine_create(&config, Aligned(g_storage.data()), kStorage - 4096, &e) == MEDIA_OK);
    if (!e) return;
    media_sensor_config_t sensor; media_sensor_config_init(&sensor);
    sensor.width = kW; sensor.height = kH; // 90 degrees across: 160 pixels a radian at the centre.
    CHECK(media_engine_open_sensor(e, &sensor) == MEDIA_OK);
    const double focal = (kW / 2.0) / std::tan(kPi / 4);
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = kW; frame.height = kH;
    frame.strides[0] = frame.strides[1] = kW; frame.flags = MEDIA_SENSOR_IMU;
    double px = 0, py = 0;
    std::uint64_t allocations = 0;
    for (unsigned f = 0; f < kFrames; ++f) {
        double sx = 0, sy = 0; Shake(Pts(f, false) / 1e7, sx, sy);
        // The IMU: yaw right moves the scene left, pitch up moves it down (the engine's convention).
        frame.imu_yaw = static_cast<float>(-(sx - px) / focal); frame.imu_pitch = static_cast<float>((sy - py) / focal); frame.imu_roll = 0;
        px = sx; py = sy;
        frame.planes[0] = nv12[f].data(); frame.planes[1] = nv12[f].data() + kW * kH; frame.timestamp_100ns = Pts(f, false);
        if (f >= 10) allocation_probe::Begin();
        CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
        if (f >= 10) allocations += allocation_probe::End();
    }
    media_frame_telemetry_t t{}; t.struct_size = sizeof(t);
    CHECK(media_query_frame_telemetry(e, &t) == MEDIA_OK);
    std::printf("SDK %s: vital %.2f BPM (conf %.2f, channel %u), vibration %.3f Hz (conf %.2f), %.2f s over [%.1f, %.1f]-[%.1f, %.1f], flags %04x, %llu allocations\n",
                signal == Signal::Pulse ? "pulse, IMU shake" : "still", t.vital_bpm, t.vital_confidence, t.vital_channel, t.vibration_hz, t.vibrometry_confidence,
                t.vibrometry_seconds, t.vibrometry_left, t.vibrometry_top, t.vibrometry_right, t.vibrometry_bottom, t.flags, static_cast<unsigned long long>(allocations));
    if (signal == Signal::Pulse) {
        CHECK(std::fabs(t.vital_bpm - 72.0f) <= 1.5f && t.vital_confidence >= 0.8f && (t.flags & MEDIA_TELEMETRY_VITALS));
        CHECK(t.vibrometry_right - t.vibrometry_left > 150 && t.vibrometry_right - t.vibrometry_left < 170 && t.vibrometry_seconds > 8);
        // API 8's struct: through the objects.
        media_frame_telemetry_t old{}; old.struct_size = 400; old.vital_bpm = 12345;
        CHECK(media_query_frame_telemetry(e, &old) == MEDIA_OK && old.vital_bpm == 12345 && old.view_zoom > 0);
    } else CHECK(t.vibration_hz < 0 && t.vital_bpm < 0 && !(t.flags & (MEDIA_TELEMETRY_VITALS | MEDIA_TELEMETRY_VIBRATION)));
    CHECK(allocations == 0);
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
}
}

// ---- 2026-11-03: a moving target, and the virtual IMU ----
// The moving target's centre at time t (samples).
void MovingAt(double t, double& x, double& y) { x = 160 + 12 * (t - 1); y = 120 + 15 * std::sin(2 * kPi * 0.15 * (t - 1)); }
// The scene with an 80 x 80 textured skin patch (rings and a stripe) pulsing at 1.2 Hz (luma +-1.5 times its light, Cr +-0.8) at MovingAt(t), lit by a
// spotlight (its light 1.0 to 1.3 times across the picture: no straight line in time as it crosses), shaken when asked.
void MakeMoving(double t, bool shake, Lcg& rng, Picture& p) {
    p.y.resize(kW * kH); p.cb.resize(kW * kH / 4); p.cr.resize(kW * kH / 4);
    double sx = 0, sy = 0; if (shake) Shake(t, sx, sy);
    double ox = 0, oy = 0; MovingAt(t, ox, oy);
    const double wave = std::sin(2 * kPi * 1.2 * t);
    const auto light = [](double x, double y) { return 1.0 + 0.3 * std::exp(-((x - 200) * (x - 200) + (y - 110) * (y - 110)) / (2 * 70.0 * 70.0)); };
    const auto inside = [&](double x, double y) { return std::fabs(x - ox) < 40 && std::fabs(y - oy) < 40; };
    for (unsigned j = 0; j < kH; ++j)
        for (unsigned i = 0; i < kW; ++i) {
            const double x = i + 0.5 - sx, y = j + 0.5 - sy;
            double v;
            if (inside(x, y)) {
                // The patch's own texture moves with it: rings about its centre and a fading stripe (a periodic texture would let
                // the tracker's template slip by a period).
                const double u = x - ox, w = y - oy, r = std::hypot(u, w);
                v = light(x, y) * (130 + 20 * std::cos(2 * kPi * r / 17.0) + 12 * std::sin(2 * kPi * (u - 0.6 * w) / 13.0) * std::exp(-r * r / 1800.0) + 1.5 * wave);
            } else v = 100 + 40 * std::sin(2 * kPi * x / 37.0) * std::cos(2 * kPi * y / 29.0) + 15 * std::sin(2 * kPi * (x - 2 * y) / 17.0);
            v += rng.Next(1);
            p.y[j * kW + i] = static_cast<std::uint8_t>(std::lround(std::fmin(255, std::fmax(0, v))));
        }
    for (unsigned j = 0; j < kH / 2; ++j)
        for (unsigned i = 0; i < kW / 2; ++i) {
            const double x = 2 * i + 1 - sx, y = 2 * j + 1 - sy;
            const bool skin = inside(x, y);
            const double cb = (skin ? 112 : 128) + rng.Next(1), cr = (skin ? 148 + 0.8 * wave : 128) + rng.Next(1);
            p.cb[j * (kW / 2) + i] = static_cast<std::uint8_t>(std::lround(cb)); p.cr[j * (kW / 2) + i] = static_cast<std::uint8_t>(std::lround(cr));
        }
}
void TestMoving() {
    std::vector<Picture> pictures(kFrames);
    Lcg rng{4242};
    for (unsigned f = 0; f < kFrames; ++f) MakeMoving(Pts(f, false) / 1e7, false, rng, pictures[f]);
    static video::Vibrometry v; static video::TargetPath path;
    // Following: the region (the fovea's size, 160 x 120) centred on the path through the tracker's jittered positions.
    v.Reset(); path.Reset();
    Lcg jitter{99};
    std::uint64_t allocations = 0;
    double worst_offset = 0;
    for (unsigned f = 0; f < kFrames; ++f) {
        const auto pts = Pts(f, false);
        double x = 0, y = 0; MovingAt(pts / 1e7, x, y);
        allocation_probe::Begin();
        path.Push(pts, x + 0.1 * jitter.Next(6), y + 0.1 * jitter.Next(6));
        double cx = x, cy = y; (void)path.At(pts, cx, cy);
        const video::Planes420 planes{pictures[f].y.data(), pictures[f].cb.data(), pictures[f].cr.data(), kW, kW / 2, kW, kH};
        v.Push(planes, {cx - 80, cy - 60, cx + 80, cy + 60}, pts, true);
        allocations += allocation_probe::End();
        if (f > 30) worst_offset = std::fmax(worst_offset, std::hypot(cx - x, cy - y));
    }
    const auto r = v.Result();
    std::printf("moving target, following: vital %.2f BPM (conf %.2f, %s), %u restarts, %.2f s window, region within %.2f samples of the target, %llu allocations\n",
                r.vital_bpm, r.vital_confidence, ChannelName(r.vital_channel), v.Restarts(), r.seconds, worst_offset, static_cast<unsigned long long>(allocations));
    CHECK(std::fabs(r.vital_bpm - 72.0f) <= 1.5f && r.vital_confidence >= 0.8f && v.Restarts() == 0 && r.seconds > 8.0f && worst_offset < 2.0 && allocations == 0);
    // The policy before: the region moved only when the target had left a third of it, each move a new window.
    v.Reset();
    double rx = -1, ry = -1; float longest = 0;
    for (unsigned f = 0; f < kFrames; ++f) {
        const auto pts = Pts(f, false);
        double x = 0, y = 0; MovingAt(pts / 1e7, x, y);
        if (rx < 0 || std::fabs(x - rx) > 160.0 / 3 || std::fabs(y - ry) > 120.0 / 3) { rx = x; ry = y; }
        const video::Planes420 planes{pictures[f].y.data(), pictures[f].cb.data(), pictures[f].cr.data(), kW, kW / 2, kW, kH};
        v.Push(planes, {rx - 80, ry - 60, rx + 80, ry + 60}, pts);
        longest = std::fmax(longest, v.Result().seconds);
    }
    std::printf("moving target, re-centred (before): vital %.2f BPM, %u restarts, longest window %.2f s\n", v.Result().vital_bpm, v.Restarts(), longest);
    CHECK(v.Restarts() >= 2 && longest < video::Vibrometry::kMinSeconds + 2);
}
// One host-mode engine fed `frames` (NV12), without IMU deltas unless `imu`; commands at given pictures; telemetry per picture.
struct HostRun {
    std::vector<media_frame_telemetry_t> telemetry;
    std::uint64_t allocations = 0, events = 0;
};
using Nv12 = std::vector<std::vector<std::uint8_t>>;
Nv12 ToNv12(const std::vector<Picture>& pictures) {
    Nv12 out(pictures.size());
    for (std::size_t f = 0; f < pictures.size(); ++f) {
        auto& o = out[f]; o.resize(kW * kH * 3 / 2);
        std::memcpy(o.data(), pictures[f].y.data(), kW * kH);
        for (unsigned i = 0; i < kW * kH / 4; ++i) { o[kW * kH + 2 * i] = pictures[f].cb[i]; o[kW * kH + 2 * i + 1] = pictures[f].cr[i]; }
    }
    return out;
}
enum : unsigned { kRunImu = 1, kRunStabilize = 2, kRunEvents = 4, kRunUnstabilizedEvents = 8, kRunTrack = 16 };
HostRun RunHost(const Nv12& frames, unsigned options, unsigned track_at = 0) {
    HostRun run;
    g_storage.assign(kStorage, 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY;
    media_engine_t* e = nullptr;
    CHECK(media_engine_create(&config, Aligned(g_storage.data()), kStorage - 4096, &e) == MEDIA_OK);
    if (!e) return run;
    media_sensor_config_t sensor; media_sensor_config_init(&sensor);
    sensor.width = kW; sensor.height = kH;
    CHECK(media_engine_open_sensor(e, &sensor) == MEDIA_OK);
    if (options & kRunStabilize) CHECK(media_engine_set_stabilization(e, 1) == MEDIA_OK);
    if (options & (kRunEvents | kRunUnstabilizedEvents)) {
        media_event_camera_config_t c; media_event_camera_config_init(&c);
        if (options & kRunUnstabilizedEvents) c.flags = MEDIA_EVENTS_UNSTABILIZED;
        CHECK(media_engine_set_event_camera(e, &c) == MEDIA_OK);
    }
    const double focal = (kW / 2.0) / std::tan(kPi / 4);
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = kW; frame.height = kH;
    frame.strides[0] = frame.strides[1] = kW; frame.flags = (options & kRunImu) ? static_cast<std::uint32_t>(MEDIA_SENSOR_IMU) : 0u;
    double px = 0, py = 0;
    static media_event_spike_t spikes[4096];
    run.telemetry.resize(frames.size());
    for (unsigned f = 0; f < frames.size(); ++f) {
        double sx = 0, sy = 0; Shake(Pts(f, false) / 1e7, sx, sy);
        frame.imu_yaw = static_cast<float>(-(sx - px) / focal); frame.imu_pitch = static_cast<float>((sy - py) / focal); frame.imu_roll = 0;
        px = sx; py = sy;
        frame.planes[0] = frames[f].data(); frame.planes[1] = frames[f].data() + kW * kH; frame.timestamp_100ns = Pts(f, false);
        if ((options & kRunTrack) && f == track_at) CHECK(media_engine_command(e, "track the target") == MEDIA_OK);
        if (f >= 10) allocation_probe::Begin();
        CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
        if (f >= 10) run.allocations += allocation_probe::End();
        auto& t = run.telemetry[f]; t = {}; t.struct_size = sizeof(t);
        (void)media_query_frame_telemetry(e, &t);
        std::uint32_t n = 0;
        while (media_poll_event_spikes(e, spikes, 4096, &n) == MEDIA_OK && n) run.events += n;
    }
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
    return run;
}
void TestSdkMoving() {
    std::vector<Picture> pictures(kFrames);
    Lcg rng{2024};
    for (unsigned f = 0; f < kFrames; ++f) MakeMoving(Pts(f, false) / 1e7, false, rng, pictures[f]);
    const auto run = RunHost(ToNv12(pictures), kRunTrack, 30); // "track" at 1 s: the target at the view's centre.
    const auto& t = run.telemetry.back();
    double x = 0, y = 0; MovingAt(Pts(kFrames - 1, false) / 1e7, x, y);
    const double cx = (t.vibrometry_left + t.vibrometry_right) / 2, cy = (t.vibrometry_top + t.vibrometry_bottom) / 2;
    std::printf("SDK moving target, tracked: vital %.2f BPM (conf %.2f), %.2f s window, following %u, region centre (%.1f, %.1f) for the target at (%.1f, %.1f), %llu allocations\n",
                t.vital_bpm, t.vital_confidence, t.vibrometry_seconds, t.vibrometry_flags & MEDIA_VIBROMETRY_FOLLOWING, cx, cy, x, y, static_cast<unsigned long long>(run.allocations));
    CHECK(std::fabs(t.vital_bpm - 72.0f) <= 1.5f && t.vital_confidence >= 0.8f && (t.vibrometry_flags & MEDIA_VIBROMETRY_FOLLOWING) && t.vibrometry_seconds >= 8.0f);
    CHECK(std::hypot(cx - x, cy - y) < 8 && run.allocations == 0);
}
void TestSdkVirtualImu() {
    // The pulse scene with the shake, recorded without IMU deltas.
    const Scene s{Signal::Pulse, 1.2, true};
    std::vector<Picture> pictures(kFrames);
    Lcg rng{777};
    for (unsigned f = 0; f < kFrames; ++f) Make(s, Pts(f, false) / 1e7, rng, pictures[f]);
    const auto frames = ToNv12(pictures);
    const auto virtual_run = RunHost(frames, kRunStabilize | kRunEvents), imu_run = RunHost(frames, kRunImu | kRunStabilize | kRunEvents), raw = RunHost(frames, kRunUnstabilizedEvents);
    // The virtual IMU's motion against the true shake's, and the stabilised view against the IMU-driven engine's.
    double motion = 0, view = 0; unsigned motions = 0, views = 0, flagged = 0;
    for (unsigned f = 1; f < kFrames; ++f) {
        const auto& t = virtual_run.telemetry[f];
        double sx = 0, sy = 0, qx = 0, qy = 0; Shake(Pts(f, false) / 1e7, sx, sy); Shake(Pts(f - 1, false) / 1e7, qx, qy);
        if (t.flags & MEDIA_TELEMETRY_VIRTUAL_IMU) { ++flagged; motion += (t.ego_dx - (sx - qx)) * (t.ego_dx - (sx - qx)) + (t.ego_dy - (sy - qy)) * (t.ego_dy - (sy - qy)); ++motions; }
        const auto& u = imu_run.telemetry[f];
        if (f > 10) { const double ex = (t.view_x - u.view_x) * kW, ey = (t.view_y - u.view_y) * kH; view += ex * ex + ey * ey; ++views; }
    }
    motion = std::sqrt(motion / (motions ? motions : 1)); view = std::sqrt(view / (views ? views : 1));
    const auto& t = virtual_run.telemetry.back();
    std::printf("SDK shaken, no IMU: virtual IMU on %u of %u pictures (%u tiles, %u background), motion %.3f samples rms off the shake; stabilised view %.3f samples rms off the IMU's; "
                "vital %.2f BPM (conf %.2f); events %llu stabilised by it, %llu by the true IMU, %llu unstabilised; %llu allocations\n",
                flagged, kFrames - 1, t.ego_tiles, t.ego_inliers, motion, view, t.vital_bpm, t.vital_confidence, static_cast<unsigned long long>(virtual_run.events),
                static_cast<unsigned long long>(imu_run.events), static_cast<unsigned long long>(raw.events), static_cast<unsigned long long>(virtual_run.allocations));
    CHECK(flagged >= kFrames - 3 && motion < 0.1 && view < 0.3 && (t.flags & MEDIA_TELEMETRY_STABILIZED));
    CHECK(std::fabs(t.vital_bpm - 72.0f) <= 1.5f && t.vital_confidence >= 0.8f);
    CHECK(raw.events > 10000 && virtual_run.events * 20 <= raw.events && virtual_run.events <= imu_run.events + imu_run.events / 5);
    CHECK(virtual_run.allocations == 0);
}

// Pictures in decode order (2026-11-04: the windowed engine measures each one before converting it, I P B B P B B ...): the same view
// as in presentation order, within 0.75 sample rms (each motion is longer), the shake gone; a pan passes through; a low-pass of the same delay as before
// leaves a third of what a one-pole did of a shake at 2.3 Hz.
void TestDecodeOrder() {
    constexpr unsigned kWide = 320, kHigh = 180, kPictures = 145;
    // A wall without a period (value noise): a periodic one lets a search of +-24 samples slip by a whole period.
    constexpr unsigned kWall = 640, kWallHigh = 400;
    static std::vector<float> texture(kWall * kWallHigh);
    const auto hash = [](std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; };
    const auto noise = [&](double u, double v, std::uint32_t seed) {
        const double fu = std::floor(u), fv = std::floor(v), a = u - fu, b = v - fv;
        const auto at = [&](int i, int j) { return static_cast<double>(hash(static_cast<std::uint32_t>(i) * 7919u + static_cast<std::uint32_t>(j) * 104729u + seed) % 256u) / 255.0; };
        const int i = static_cast<int>(fu), j = static_cast<int>(fv);
        return (1 - a) * (1 - b) * at(i, j) + a * (1 - b) * at(i + 1, j) + (1 - a) * b * at(i, j + 1) + a * b * at(i + 1, j + 1);
    };
    for (unsigned y = 0; y < kWallHigh; ++y)
        for (unsigned x = 0; x < kWall; ++x) texture[y * kWall + x] = static_cast<float>(40 + 160 * (0.5 * noise(x / 24.0, y / 24.0, 3) + 0.3 * noise(x / 9.0, y / 9.0, 5) + 0.2 * noise(x / 3.5, y / 3.5, 7)));
    const auto wall = [&](double x, double y) {
        const int ix = static_cast<int>(x), iy = static_cast<int>(y);
        const double a = x - ix, b = y - iy;
        const float* p = texture.data() + std::size_t(iy) * kWall + ix;
        return (1 - a) * (1 - b) * p[0] + a * (1 - b) * p[1] + (1 - a) * b * p[kWall] + a * b * p[kWall + 1];
    };
    const auto shake_x = [&](double t) { return 6 * std::sin(2 * kPi * 2.3 * t) + 4 * std::sin(2 * kPi * 4.1 * t + 1) + 10 * t; };
    const auto shake_y = [&](double t) { return 5 * std::sin(2 * kPi * 2.9 * t + 0.5) + 3 * std::sin(2 * kPi * 5.3 * t); };
    std::vector<std::vector<std::uint8_t>> pictures(kPictures, std::vector<std::uint8_t>(kWide * kHigh));
    for (unsigned k = 0; k < kPictures; ++k) {
        const double t = k / 24.0, dx = shake_x(t), dy = shake_y(t);
        for (unsigned y = 0; y < kHigh; ++y)
            for (unsigned x = 0; x < kWide; ++x) pictures[k][y * kWide + x] = static_cast<std::uint8_t>(std::clamp(std::lround(wall(x + dx + 100, y + dy + 100)), 16l, 235l));
    }
    const auto run = [&](const std::vector<unsigned>& order, bool decoded, std::vector<float>& view_x, std::vector<float>& view_y) {
        core::LinearMemoryArena arena; (void)arena.Initialize(4u << 20);
        video::VirtualImu imu; video::Stabilizer steady;
        (void)imu.Configure(arena, kWide, kHigh, 0); steady.Configure(kWide, kHigh, 0);
        view_x.assign(kPictures, 0); view_y.assign(kPictures, 0);
        std::int64_t newest = INT64_MIN; bool reversed = false;
        for (const unsigned k : order) {
            const auto pts = static_cast<std::int64_t>(k) * 416667;
            const bool back = pts < newest;
            if (back || reversed) imu.ForgetMotion();
            reversed = back; newest = (std::max)(newest, pts);
            (void)imu.Push(pictures[k].data(), kWide);
            if (decoded) steady.PushDecoded(pts, imu.Ego()); else steady.Push(pts, imu.Ego());
            float zoom = 1, x = 0.5f, y = 0.5f, roll = 0;
            steady.Apply(zoom, x, y, roll);
            view_x[k] = x * kWide; view_y[k] = y * kHigh;
        }
    };
    std::vector<unsigned> presentation, decode;
    for (unsigned k = 0; k < kPictures; ++k) presentation.push_back(k);
    decode.push_back(0);
    for (unsigned k = 3; k < kPictures; k += 3) { decode.push_back(k); decode.push_back(k - 2); decode.push_back(k - 1); }
    std::vector<float> ax, ay, bx, by;
    run(presentation, false, ax, ay); run(decode, true, bx, by);
    double apart = 0, shaken = 0, sum_x = 0, sum_y = 0, square = 0; unsigned n = 0;
    for (unsigned k = 36; k + 3 < decode.size() && k < kPictures - 3; ++k) {
        const double t = k / 24.0;
        apart += std::pow(ax[k] - bx[k], 2) + std::pow(ay[k] - by[k], 2);
        // What is left on screen of the shake: the content moved by -s, the view with it; about its mean (a steady pan's lag).
        const double sx = shake_x(t) - 10 * t, sy = shake_y(t), ex = bx[k] - kWide / 2.0 + sx, ey = by[k] - kHigh / 2.0 + sy;
        sum_x += ex; sum_y += ey; square += ex * ex + ey * ey;
        shaken += sx * sx + sy * sy; ++n;
    }
    apart = std::sqrt(apart / n); shaken = std::sqrt(shaken / n);
    const double residual = std::sqrt((std::max)(square / n - (sum_x * sum_x + sum_y * sum_y) / (static_cast<double>(n) * n), 0.0));
    std::printf("Decode order: %u pictures (I P B B ...), the view within %.3f samples rms of presentation order's; the shake %.2f samples rms, %.2f left on the view\n", n, apart, shaken, residual);
    CHECK(apart <= 0.75 && residual <= 0.25 * shaken);
    // The low-pass: what it keeps of a 2.3 Hz shake (at 240 pictures a second, the input's shape), and of a pan.
    video::Stabilizer steady; steady.Configure(1000, 1000, 0);
    double kept = 0, last_yaw = 0, lag = 0;
    for (unsigned k = 0; k < 2400; ++k) {
        const double t = k / 240.0, yaw = 0.01 * std::sin(2 * kPi * 2.3 * t);
        video::EgoMotion ego; ego.yaw = static_cast<float>(yaw - last_yaw); ego.valid = true; last_yaw = yaw;
        steady.Push(static_cast<std::int64_t>(k) * 41667, ego);
        if (k >= 1200) kept = (std::max)(kept, std::fabs(yaw + static_cast<double>(steady.ShiftX()) * 1000 / steady.Focal()) / 0.01);
    }
    steady.Reset();
    for (unsigned k = 0; k < 1200; ++k) { // A steady pan: the view lags it by the delay, and no more.
        video::EgoMotion ego; ego.yaw = 0.01f / 240; ego.valid = true;
        steady.Push(static_cast<std::int64_t>(k) * 41667, ego);
        if (k == 1199) lag = -static_cast<double>(steady.ShiftX()) * 1000 / steady.Focal() / 0.01;
    }
    std::printf("Low-pass: keeps %.3f of a 2.3 Hz shake (a one-pole of the same delay: 0.194); a pan lags %.3f s\n", kept, lag);
    CHECK(kept <= 0.09 && std::fabs(lag - video::Stabilizer::kSeconds) <= 0.01);
}
int main() {
    std::puts("Vibrometry tests"); // stdout's buffer before the first probe (MSVC Debug counts the CRT's).
    TestClass();
    TestSdk(Signal::Pulse);
    TestSdk(Signal::Still);
    TestMoving();
    TestSdkMoving();
    TestSdkVirtualImu();
    TestDecodeOrder();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "vibrometry tests passed", failures);
    return failures ? 1 : 0;
}
