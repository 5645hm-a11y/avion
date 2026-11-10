// The neuromorphic event camera (docs/picture.md, "Event camera"; docs/sdk.md, API 9), 2026-11-02. Suite #202.
// Portable (MSVC; GCC -O2 and ASan/UBSan). Synthetic 320 x 240 pictures at 30 per second: a dark, smoothly textured background
// and a bright 40 x 40 square with soft edges (a lens's blur: a 1.5-sample ramp), sampled where the scene lies under the
// camera (its shake: 3 px at 2.3 Hz + 1.5 px at 5.1 Hz across, 2.5 px at 3.7 Hz down, and a roll of 0.01 rad at 1.7 Hz), its
// IMU motion given to the camera picture by picture.
// video::EventCamera:
// - the square moving right 3 samples a picture on a still camera: ON events (+1) on its leading edge, OFF events (-1) on
//   its trailing edge, 97 % or more of either on its own edge (in the band the edge swept, a sample of blur either side),
//   none elsewhere; each picture's events in time order (timestamps between the two pictures);
// - a still scene under the shake (rolling too): stabilised, at most 0.1 % of the events the same sensor fires unstabilised,
//   and fewer than 0.05 % of the pixels a picture (the square's 160-code border fired 0.05 % before the registration guard;
//   with bilinear sampling 0.08 %, interpolating logs 0.18 %);
// - a still scene while the camera pans and banks (135 px, 0.11 rad over 90 pictures): the scene's frame moved by whole
//   pixels and started over after its turns (15 rebases or more), fewer than 0.01 % of the pixels a picture fire;
// - the moving square under the shake: stabilised, still 90 % or more on the right edges with the right polarity;
// The pictures are integrated over each pixel's area (2 x 2 points), as a sensor's photosites are: point samples of a
// 1.5-sample edge are aliased beyond any interpolation's reach (the 1280 x 720 SDK pictures are point samples: the event
// sensor averages 2 x 2 of them).
// - the ring: unread, it holds 65,536 events and counts the rest dropped (the latest of a picture's first), then gives the
//   oldest in order; no allocation in Push or Poll; the cost per picture;
// - a 4K camera's sensor is 640 x 360 (6 x 6 samples a pixel).
// The SDK in host mode with a 1280 x 720 camera (a 640 x 360 sensor): media_engine_set_event_camera, the shake through the
// IMU, the moving square: media_poll_event_spikes gives ON / OFF events on its edges (scaled), timestamps never going back,
// media_query_event_camera's counts; MEDIA_EVENTS_UNSTABILIZED fires many more; off: nothing more; no allocation.
// 2026-11-03 (API 10; Windows): the windowed engine's event camera, on its tracking grids, on the two-targets fixture (a still
// camera, two textured squares approaching; hidden, 3 s): media_engine_set_event_camera after open, events polled from the
// frame callback: hundreds of them, at least 98 % within 3 cells of the squares (their known boxes at the events' times), none
// dropped, the sensor 320 x 240, the state's pictures and counts (on perception's thread: 50 grids or more in optimised builds,
// 20 in Debug, which skips those that come while the thread is busy), no picture dropped; no allocation in the polls.
#include "media/c_api.h"
#include "media/engine/EngineModules.hpp"
#include "media/video/EventCamera.hpp"
#include "media/core/linear_memory_arena.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr double kPi = 3.14159265358979323846;
constexpr double kFps = 30.0;

struct Motion { double x = 0, y = 0, roll = 0; };
Motion Shake(double t, bool on) {
    if (!on) return {};
    return {3.0 * std::sin(2 * kPi * 2.3 * t) + 1.5 * std::sin(2 * kPi * 5.1 * t), 2.5 * std::sin(2 * kPi * 3.7 * t), 0.01 * std::sin(2 * kPi * 1.7 * t)};
}
double Ramp(double d) { return std::fmin(1.0, std::fmax(0.0, d / 1.5 + 0.5)); } // A soft edge: 0 outside, 1 inside, 1.5 samples wide.
// The scene at world point (x, y): a dark smooth texture, a bright square [left, left + 40) x [top, top + 40).
double World(double x, double y, double left, double top) {
    const double background = 40 + 12 * std::sin(2 * kPi * x / 41.0) * std::cos(2 * kPi * y / 33.0) + 6 * std::sin(2 * kPi * (x + 2 * y) / 57.0);
    const double inside = Ramp(x - left) * Ramp(left + 40 - x) * Ramp(y - top) * Ramp(top + 40 - y);
    return background + inside * (200 - background);
}
// A picture of the world as the camera sees it: pixel p shows the world point w with p = R(roll)(w - c) + c + s, integrated
// over the pixel's area as a sensor's photosite does (`points` x `points`; 1: point samples, for a picture the event sensor
// box-averages anyway).
void Render(std::uint8_t* out, unsigned w, unsigned h, unsigned stride, double left, double top, const Motion& m, double sample_scale = 1, unsigned points = 2) {
    const double cx = w / 2.0, cy = h / 2.0, c = std::cos(-m.roll), s = std::sin(-m.roll);
    for (unsigned j = 0; j < h; ++j)
        for (unsigned i = 0; i < w; ++i) {
            double sum = 0;
            for (unsigned b = 0; b < points; ++b)
                for (unsigned a = 0; a < points; ++a) {
                    const double px = i + (a + 0.5) / points - cx - m.x, py = j + (b + 0.5) / points - cy - m.y;
                    sum += World((c * px - s * py + cx) / sample_scale, (s * px + c * py + cy) / sample_scale, left, top);
                }
            out[static_cast<std::size_t>(j) * stride + i] = static_cast<std::uint8_t>(std::lround(sum / (points * points)));
        }
}
// The camera's motion between two pictures as EventCamera takes it: p_k = R(r)(p_{k-1} - c) + c + d with r = roll_k - roll_{k-1}
// and d = s_k - R(r) s_{k-1}.
void Delta(const Motion& a, const Motion& b, float& dx, float& dy, float& roll) {
    const double r = b.roll - a.roll, c = std::cos(r), s = std::sin(r);
    dx = static_cast<float>(b.x - (c * a.x - s * a.y)); dy = static_cast<float>(b.y - (s * a.x + c * a.y)); roll = static_cast<float>(r);
}
std::int64_t Pts(unsigned f) { return static_cast<std::int64_t>(std::llround(f / kFps * 1e7)); }

constexpr unsigned kW = 320, kH = 240;
alignas(64) unsigned char g_arena_storage[8u << 20];
video::EventSpike g_events[1u << 17];

struct EdgeScore { std::uint64_t on = 0, off = 0, on_right = 0, off_right = 0, elsewhere = 0, out_of_order = 0; };
// The square moving right `speed` samples a picture from left 60, top 100; `shake`: the camera moves; `stabilized` as asked.
EdgeScore Moving(bool shake, bool stabilized, double speed, double& us_per_picture, std::uint64_t& allocations) {
    core::LinearMemoryArena arena;
    CHECK(arena.InitializeView(g_arena_storage, sizeof(g_arena_storage)));
    static video::EventCamera camera; CHECK(camera.Configure(arena, kW, kH));
    video::EventCameraOptions o; o.stabilized = stabilized; camera.SetOptions(o);
    constexpr unsigned kPictures = 40;
    std::vector<std::vector<std::uint8_t>> pictures(kPictures, std::vector<std::uint8_t>(kW * kH));
    for (unsigned f = 0; f < kPictures; ++f) Render(pictures[f].data(), kW, kH, kW, 60 + speed * f, 100, Shake(f / kFps, shake));
    EdgeScore score;
    double elapsed = 0; allocations = 0;
    for (unsigned f = 0; f < kPictures; ++f) {
        float dx = 0, dy = 0, roll = 0;
        if (f) Delta(Shake((f - 1) / kFps, shake), Shake(f / kFps, shake), dx, dy, roll);
        const auto start = std::chrono::steady_clock::now();
        allocation_probe::Begin();
        (void)camera.Push(pictures[f].data(), kW, Pts(f), dx, dy, roll);
        const auto n = camera.Poll(g_events, 1u << 17);
        allocations += allocation_probe::End();
        elapsed += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        // Where the square's edges swept between f - 1 and f, in the sensor's frame (the scene's: the camera's at the first picture,
        // whose shake is 0 at t = 0), a sample of blur either side; its rows with the same margin.
        const double was = 60 + speed * (f ? f - 1 : 0), now = 60 + speed * f;
        std::int64_t last = INT64_MIN;
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto& e = g_events[i];
            if (e.timestamp_us < last) ++score.out_of_order;
            last = e.timestamp_us;
            CHECK(e.timestamp_us > Pts(f - 1) / 10 && e.timestamp_us <= Pts(f) / 10);
            const double x = e.x + 0.5, y = e.y + 0.5;
            const bool rows = y >= 100 - 1.5 && y <= 140 + 1.5;
            const bool leading = rows && x >= was + 40 - 1.5 && x <= now + 40 + 1.5, trailing = rows && x >= was - 1.5 && x <= now + 1.5;
            if (e.polarity > 0) { ++score.on; if (leading) ++score.on_right; else if (!trailing) ++score.elsewhere; }
            else { ++score.off; if (trailing) ++score.off_right; else if (!leading) ++score.elsewhere; }
        }
    }
    us_per_picture = elapsed / kPictures;
    return score;
}
// A still scene (the square held) under the shake: the events a picture, stabilised or not.
std::uint64_t Still(bool stabilized, std::uint32_t& rebases, std::uint64_t& on_edges) {
    core::LinearMemoryArena arena;
    CHECK(arena.InitializeView(g_arena_storage, sizeof(g_arena_storage)));
    static video::EventCamera camera; CHECK(camera.Configure(arena, kW, kH));
    video::EventCameraOptions o; o.stabilized = stabilized; camera.SetOptions(o);
    std::vector<std::uint8_t> picture(kW * kH);
    std::uint64_t events = 0; on_edges = 0;
    for (unsigned f = 0; f < 90; ++f) {
        Render(picture.data(), kW, kH, kW, 140, 100, Shake(f / kFps, true));
        float dx = 0, dy = 0, roll = 0;
        if (f) Delta(Shake((f - 1) / kFps, true), Shake(f / kFps, true), dx, dy, roll);
        events += camera.Push(picture.data(), kW, Pts(f), dx, dy, roll);
        const auto n = camera.Poll(g_events, 1u << 17);
        // Within 3 samples of the square's border (its 160-code edges).
        for (std::uint32_t i = 0; i < n; ++i) {
            const double x = g_events[i].x + 0.5, y = g_events[i].y + 0.5;
            const bool border = x > 137 && x < 183 && y > 97 && y < 143, inside = x > 143 && x < 177 && y > 103 && y < 137;
            if (border && !inside) ++on_edges;
        }
    }
    rebases = camera.Rebases();
    return events;
}
// A still scene while the camera pans (1.5 px a picture across, 0.4 down) and banks (0.0012 rad a picture): the scene's frame is
// moved by whole pixels every 6 and started over after 0.03 rad; the events a picture.
std::uint64_t Pan(std::uint32_t& rebases) {
    core::LinearMemoryArena arena;
    CHECK(arena.InitializeView(g_arena_storage, sizeof(g_arena_storage)));
    static video::EventCamera camera; CHECK(camera.Configure(arena, kW, kH));
    std::vector<std::uint8_t> picture(kW * kH);
    const auto at = [](unsigned f) { return Motion{1.5 * f, 0.4 * f, 0.0012 * f}; };
    std::uint64_t events = 0;
    for (unsigned f = 0; f < 90; ++f) {
        Render(picture.data(), kW, kH, kW, 140, 100, at(f));
        float dx = 0, dy = 0, roll = 0;
        if (f) Delta(at(f - 1), at(f), dx, dy, roll);
        events += camera.Push(picture.data(), kW, Pts(f), dx, dy, roll);
        (void)camera.Poll(g_events, 1u << 17);
    }
    rebases = camera.Rebases();
    return events;
}
void TestClass() {
    double us = 0; std::uint64_t allocations = 0;
    const auto still_camera = Moving(false, true, 3.0, us, allocations);
    std::printf("Moving square, still camera: %llu ON (%llu on the leading edge), %llu OFF (%llu on the trailing edge), %llu elsewhere, %llu out of order; %.0f us a picture, %llu allocations\n",
                static_cast<unsigned long long>(still_camera.on), static_cast<unsigned long long>(still_camera.on_right), static_cast<unsigned long long>(still_camera.off),
                static_cast<unsigned long long>(still_camera.off_right), static_cast<unsigned long long>(still_camera.elsewhere),
                static_cast<unsigned long long>(still_camera.out_of_order), us, static_cast<unsigned long long>(allocations));
    CHECK(still_camera.on > 1000 && still_camera.off > 1000);
    CHECK(still_camera.on_right * 100 >= still_camera.on * 97 && still_camera.off_right * 100 >= still_camera.off * 97 && still_camera.elsewhere == 0);
    CHECK(still_camera.out_of_order == 0 && allocations == 0);
    std::uint32_t rebases = 0, raw_rebases = 0; std::uint64_t edges = 0, raw_edges = 0;
    const auto stabilized = Still(true, rebases, edges), raw = Still(false, raw_rebases, raw_edges);
    std::printf("Still scene under the shake (rolling): %llu events stabilised (%llu on the square's border, %u rebases), %llu unstabilised (%.3f %% of the pixels a picture stabilised)\n",
                static_cast<unsigned long long>(stabilized), static_cast<unsigned long long>(edges), rebases, static_cast<unsigned long long>(raw),
                100.0 * stabilized / (89.0 * kW * kH));
    CHECK(raw > 20000 && stabilized * 1000 <= raw && stabilized * 2000 < 89ull * kW * kH);
    std::uint32_t pan_rebases = 0;
    const auto panned = Pan(pan_rebases);
    std::printf("Still scene, panning and banking camera: %llu events stabilised, %u rebases\n", static_cast<unsigned long long>(panned), pan_rebases);
    CHECK(pan_rebases >= 15 && panned * 10000 < 89ull * kW * kH);
    const auto shaken = Moving(true, true, 3.0, us, allocations);
    std::printf("Moving square under the shake: %llu ON (%llu right), %llu OFF (%llu right), %llu elsewhere\n", static_cast<unsigned long long>(shaken.on),
                static_cast<unsigned long long>(shaken.on_right), static_cast<unsigned long long>(shaken.off), static_cast<unsigned long long>(shaken.off_right),
                static_cast<unsigned long long>(shaken.elsewhere));
    CHECK(shaken.on > 1000 && shaken.off > 1000 && shaken.on_right * 10 >= shaken.on * 9 && shaken.off_right * 10 >= shaken.off * 9);
    CHECK(allocations == 0);
    {
        // The ring, unread: full at 65,536, the rest dropped; then the oldest in order.
        core::LinearMemoryArena arena; CHECK(arena.InitializeView(g_arena_storage, sizeof(g_arena_storage)));
        static video::EventCamera camera; CHECK(camera.Configure(arena, kW, kH));
        std::vector<std::uint8_t> a(kW * kH), b(kW * kH);
        for (unsigned i = 0; i < kW * kH; ++i) { a[i] = static_cast<std::uint8_t>(i % 2 ? 40 : 200); b[i] = static_cast<std::uint8_t>(i % 2 ? 200 : 40); }
        for (unsigned f = 0; f < 6; ++f) (void)camera.Push(f % 2 ? b.data() : a.data(), kW, Pts(f), 0, 0, 0);
        std::uint64_t taken = 0; std::int64_t last = INT64_MIN; bool ordered = true;
        allocation_probe::Begin();
        while (const auto n = camera.Poll(g_events, 1000)) { for (std::uint32_t i = 0; i < n; ++i) { ordered = ordered && g_events[i].timestamp_us >= last; last = g_events[i].timestamp_us; } taken += n; }
        const auto polled = allocation_probe::End();
        std::printf("Ring: %llu written, %llu dropped, %llu taken, in order %s\n", static_cast<unsigned long long>(camera.Emitted()),
                    static_cast<unsigned long long>(camera.Dropped()), static_cast<unsigned long long>(taken), ordered ? "yes" : "NO");
        CHECK(camera.Emitted() == video::EventCamera::kRing && taken == video::EventCamera::kRing && camera.Dropped() > 0 && ordered && polled == 0);
        CHECK(camera.Emitted() + camera.Dropped() == 5ull * kW * kH * 4); // 4 crossings a pixel (log 40 -> 200 is ~2.9 / 0.15) each picture.
    }
    {
        core::LinearMemoryArena arena; CHECK(arena.InitializeView(g_arena_storage, sizeof(g_arena_storage)));
        static video::EventCamera camera; CHECK(camera.Configure(arena, 3840, 2160));
        CHECK(camera.Width() == 640 && camera.Height() == 360 && camera.Scale() == 6 && video::EventCamera::Bytes(4096, 2304) < (2u << 20));
    }
}

// The SDK: a 1280 x 720 camera (a 640 x 360 sensor, 2 x 2 samples a pixel).
constexpr std::size_t kStorage = 48u << 20;
std::vector<unsigned char> g_storage;
void* Aligned(unsigned char* base) {
    const std::size_t a = media_engine_storage_alignment();
    return base + (a - reinterpret_cast<std::uintptr_t>(base) % a) % a;
}
media_event_spike_t g_spikes[1u << 16];
// The ring's packed events (2026-11-04: 8 bytes each, a 36-bit timestamp): what Poll hands back is what the 16-byte ring held, across
// the low bits' wrap and a seek back; a smaller ring; the module's memory.
void TestPackedRing() {
    constexpr unsigned kWide = 64, kHigh = 48;
    core::LinearMemoryArena arena; CHECK(arena.InitializeView(g_arena_storage, sizeof(g_arena_storage)));
    static video::EventCamera camera; CHECK(camera.Configure(arena, kWide, kHigh, 1024));
    video::EventCameraOptions options; options.stabilized = false; options.max_per_pixel = 1; camera.SetOptions(options);
    std::vector<std::uint8_t> dark(kWide * kHigh, 60), bright(kWide * kHigh, 200);
    static video::EventSpike out[4096];
    // The sensor's clock just under 2^36 microseconds: 19 hours in; the events after the wrap keep their high bits.
    const std::int64_t wrap = (std::int64_t{1} << 36) * 10; // 100 ns.
    (void)camera.Push(dark.data(), kWide, wrap - 400000, 0, 0, 0);
    const auto first = camera.Push(bright.data(), kWide, wrap - 200000, 0, 0, 0);
    const auto got_first = camera.Poll(out, 4096);
    bool before = got_first == first && first > 0;
    for (unsigned i = 0; i < got_first; ++i) before = before && out[i].timestamp_us > (wrap - 400000) / 10 && out[i].timestamp_us <= (wrap - 200000) / 10 && out[i].polarity == 1 && out[i].contrast > 0 && out[i].reserved == 0;
    const auto second = camera.Push(dark.data(), kWide, wrap + 200000, 0, 0, 0);
    const auto got_second = camera.Poll(out, 4096);
    bool after = got_second == second && second > 0;
    for (unsigned i = 0; i < got_second; ++i) after = after && out[i].timestamp_us > (wrap - 200000) / 10 && out[i].timestamp_us <= (wrap + 200000) / 10 && out[i].polarity == -1;
    // A seek back: unpolled events keep their later time (the newest is earlier than they are).
    const auto late = camera.Push(bright.data(), kWide, wrap + 400000, 0, 0, 0);
    camera.Reset();
    (void)camera.Push(dark.data(), kWide, 1000000, 0, 0, 0);
    const auto early = camera.Push(bright.data(), kWide, 1200000, 0, 0, 0);
    const auto got_mixed = camera.Poll(out, 4096);
    bool mixed = got_mixed == late + early || got_mixed == 1024;
    for (unsigned i = 0; i < (std::min)(got_mixed, late); ++i) mixed = mixed && out[i].timestamp_us > (wrap + 200000) / 10;
    std::printf("Packed ring: %u events to %lld us, %u after the wrap to %lld us, %u then %u past a seek back; %llu bytes at 480 x 272 with 32,768 events, %llu with 65,536\n",
                first, static_cast<long long>((wrap - 200000) / 10), second, static_cast<long long>((wrap + 200000) / 10), late, early,
                static_cast<unsigned long long>(video::EventCamera::Bytes(480, 272, 1u << 15)), static_cast<unsigned long long>(video::EventCamera::Bytes(480, 272)));
    CHECK(before && after && mixed);
    CHECK(!camera.Configure(arena, kWide, kHigh, 1000) && !camera.Configure(arena, kWide, kHigh, 128)); // A power of two, at least 256.
    CHECK(video::EventCamera::Bytes(480, 272, 1u << 15) <= 800u << 10 && engine::kModuleBudgets[5].arena_bytes < 1536u << 10);
}
void TestSdk() {
    constexpr unsigned W = 1280, H = 720, kPictures = 40;
    g_storage.assign(kStorage, 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY;
    media_engine_t* e = nullptr;
    CHECK(media_engine_create(&config, Aligned(g_storage.data()), kStorage - 4096, &e) == MEDIA_OK);
    if (!e) return;
    media_sensor_config_t sensor; media_sensor_config_init(&sensor);
    sensor.width = W; sensor.height = H;
    CHECK(media_engine_open_sensor(e, &sensor) == MEDIA_OK);
    media_event_camera_config_t ec; media_event_camera_config_init(&ec);
    CHECK(ec.contrast_threshold == 0.15f && ec.max_events_per_pixel == 4);
    CHECK(media_engine_set_event_camera(e, &ec) == MEDIA_OK);
    media_event_camera_state_t st{}; st.struct_size = sizeof(st);
    CHECK(media_query_event_camera(e, &st) == MEDIA_OK && st.on == 1 && st.width == 640 && st.height == 360 && st.scale == 2);
    const double focal = (W / 2.0) / std::tan(kPi / 4);
    // The scene drawn at the picture's scale (the world in sensor pixels x 2: the square 80 x 80 samples, moving 6 a picture).
    std::vector<std::vector<std::uint8_t>> pictures(kPictures, std::vector<std::uint8_t>(W * H * 3 / 2, 128));
    for (unsigned f = 0; f < kPictures; ++f) {
        auto m = Shake(f / kFps, true); m.x *= 2; m.y *= 2;
        Render(pictures[f].data(), W, H, W, 60 + 3.0 * f, 100, m, 2.0, 1);
    }
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = W; frame.height = H;
    frame.strides[0] = frame.strides[1] = W; frame.flags = MEDIA_SENSOR_IMU;
    std::uint64_t on = 0, off = 0, right = 0, allocations = 0, total = 0; std::int64_t last = INT64_MIN; bool ordered = true;
    for (unsigned stage = 0; stage < 2; ++stage) {
        if (stage == 1) { // Again unstabilised: the shake fires.
            ec.flags = MEDIA_EVENTS_UNSTABILIZED; CHECK(media_engine_set_event_camera(e, &ec) == MEDIA_OK);
        }
        std::uint64_t stage_events = 0;
        for (unsigned f = 0; f < kPictures; ++f) {
            const unsigned k = stage * kPictures + f;
            const auto a = Shake((f ? f - 1 : 0) / kFps, true), b = Shake(f / kFps, true);
            // The IMU at the centre: yaw turns the scene left, pitch down; roll clockwise turns it by minus the roll.
            frame.imu_yaw = static_cast<float>(-2 * (b.x - a.x) / focal); frame.imu_pitch = static_cast<float>(2 * (b.y - a.y) / focal);
            frame.imu_roll = static_cast<float>(-(b.roll - a.roll));
            frame.planes[0] = pictures[f].data(); frame.planes[1] = pictures[f].data() + W * H; frame.timestamp_100ns = Pts(k);
            allocation_probe::Begin();
            CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
            std::uint32_t n = 0;
            const auto status = media_poll_event_spikes(e, g_spikes, 1u << 16, &n);
            allocations += allocation_probe::End();
            CHECK(status == (n ? MEDIA_OK : MEDIA_ERROR_EMPTY));
            stage_events += n;
            if (stage == 0 && f) {
                const double was = 60 + 3.0 * (f - 1), now = 60 + 3.0 * f;
                for (std::uint32_t i = 0; i < n; ++i) {
                    const auto& s = g_spikes[i];
                    ordered = ordered && s.timestamp_us >= last; last = s.timestamp_us;
                    const double x = s.x + 0.5, y = s.y + 0.5;
                    const bool rows = y >= 98 && y <= 142;
                    if (s.polarity > 0) { ++on; if (rows && x >= was + 38 && x <= now + 42) ++right; }
                    else { ++off; if (rows && x >= was - 2 && x <= now + 2) ++right; }
                }
            }
        }
        total += stage_events;
        if (stage == 0) {
            std::printf("SDK stabilised: %llu ON, %llu OFF, %.1f %% on their edges, in order %s\n", static_cast<unsigned long long>(on), static_cast<unsigned long long>(off),
                        on + off ? 100.0 * right / (on + off) : 0.0, ordered ? "yes" : "NO");
            CHECK(on > 500 && off > 500 && right * 10 >= (on + off) * 9 && ordered);
            CHECK(media_query_event_camera(e, &st) == MEDIA_OK && st.events == total && st.dropped == 0); // Polled every picture: room.
        } else {
            std::printf("SDK unstabilised: %llu events (stabilised: %llu)\n", static_cast<unsigned long long>(stage_events), static_cast<unsigned long long>(on + off));
            CHECK(stage_events > 3 * (on + off));
        }
    }
    CHECK(media_query_event_camera(e, &st) == MEDIA_OK);
    std::printf("SDK state: %llu events (%llu taken), %llu dropped, %u pictures, %u rebases, last %u\n", static_cast<unsigned long long>(st.events),
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(st.dropped), st.pictures, st.rebases, st.last_events);
    CHECK(st.events == total && st.pictures == 2 * kPictures); // Unstabilised, a picture may fire more than the ring holds.
    CHECK(allocations == 0);
    CHECK(media_engine_set_event_camera(e, nullptr) == MEDIA_OK);
    frame.timestamp_100ns = Pts(2 * kPictures); frame.planes[0] = pictures[0].data(); frame.planes[1] = pictures[0].data() + W * H;
    CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
    std::uint32_t n = 7;
    CHECK(media_poll_event_spikes(e, g_spikes, 16, &n) == MEDIA_ERROR_EMPTY && n == 0);
    CHECK(media_query_event_camera(e, &st) == MEDIA_OK && st.on == 0);
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
}

#if defined(_WIN32)
// The fixture's path as UTF-8 from the wide command line (argv is in the ANSI code page: "Développement" would not survive).
void FixtureArgument(char* fixture, std::size_t capacity) {
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide && count > 1) (void)WideCharToMultiByte(CP_UTF8, 0, wide[1], -1, fixture, static_cast<int>(capacity), nullptr, nullptr);
    if (wide) LocalFree(wide);
}
#endif
#if defined(_WIN32)
struct WindowEvents {
    media_engine_t* e = nullptr;
    unsigned frames = 0;
    std::uint64_t events = 0, on = 0, background = 0, allocations = 0;
    bool time_order = true; std::int64_t last_us = INT64_MIN;
};
WindowEvents g_window;
media_event_spike_t g_window_spikes[4096];
// Whether an event of the two-targets fixture lies on one of its squares (their half sizes grow as they approach), within margin cells.
bool OnSquare(double x, double y, double t, double margin) {
    static constexpr double cx[2]{0.27 * 320, 0.73 * 320}, cy[2]{0.42 * 240, 0.58 * 240};
    for (int k = 0; k < 2; ++k) {
        const double h = k == 0 ? 14 * 4.0 / (4.0 - 0.45 * t) : 18 * 5.0 / (5.0 - 0.60 * t);
        if (std::fabs(x + 0.5 - cx[k]) <= h + margin && std::fabs(y + 0.5 - cy[k]) <= h + margin) return true;
    }
    return false;
}
void OnWindowFrame(void* context, const media_frame_signature_t*) {
    auto& w = *static_cast<WindowEvents*>(context);
    ++w.frames;
    allocation_probe::Begin();
    std::uint32_t n = 0;
    while (media_poll_event_spikes(w.e, g_window_spikes, 4096, &n) == MEDIA_OK && n)
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto& s = g_window_spikes[i];
            ++w.events;
            if (OnSquare(s.x, s.y, s.timestamp_us / 1e6, 3)) ++w.on; else ++w.background;
            if (s.timestamp_us < w.last_us) w.time_order = false;
            w.last_us = s.timestamp_us;
        }
    w.allocations += allocation_probe::End();
}
void TestWindow(const char* fixture) {
    if (!fixture || !*fixture) { std::puts("Window: no fixture given, skipped"); return; }
    static std::vector<unsigned char> storage;
    storage.assign(media_engine_storage_bytes() + media_engine_storage_alignment(), 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.visible = 0; config.maximum_seconds = 3; config.wake_policy = MEDIA_WAKE_DISABLED;
    config.on_frame = &OnWindowFrame; config.on_frame_context = &g_window;
    media_engine_t* e = nullptr;
    const std::size_t a = media_engine_storage_alignment();
    CHECK(media_engine_create(&config, storage.data() + (a - reinterpret_cast<std::uintptr_t>(storage.data()) % a) % a, media_engine_storage_bytes(), &e) == MEDIA_OK);
    if (!e) return;
    g_window = {}; g_window.e = e;
    CHECK(media_engine_open(e, fixture) == MEDIA_OK);
    media_event_camera_config_t c; media_event_camera_config_init(&c);
    CHECK(media_engine_set_event_camera(e, &c) == MEDIA_OK);
    std::puts("Window: the two-targets fixture, hidden, 3 s, the event camera on");
    CHECK(media_engine_run(e) == MEDIA_OK);
    media_event_camera_state_t st{}; st.struct_size = sizeof(st);
    CHECK(media_query_event_camera(e, &st) == MEDIA_OK);
    media_engine_stats_t stats{};
    CHECK(media_engine_stats(e, &stats) == MEDIA_OK);
    const auto& w = g_window;
    std::printf("Window: %u pictures, sensor %ux%u (scale %u, on %d): %llu events polled (%llu counted, %llu dropped, %u pictures compared), %.2f %% on the squares, "
                "%llu off them, time order %s; %u presented, %u dropped; %llu allocations in the polls\n", w.frames, st.width, st.height, st.scale, st.on,
                static_cast<unsigned long long>(w.events), static_cast<unsigned long long>(st.events), static_cast<unsigned long long>(st.dropped), st.pictures,
                w.events ? 100.0 * static_cast<double>(w.on) / static_cast<double>(w.events) : 0.0, static_cast<unsigned long long>(w.background),
                w.time_order ? "kept" : "BROKEN", stats.presented, stats.dropped, static_cast<unsigned long long>(w.allocations));
    // Every grid in optimised builds; unoptimised, perception's thread skips those that come while it is busy (no picture waits).
#if defined(NDEBUG)
    const unsigned compared = 50;
#else
    const unsigned compared = 20;
#endif
    CHECK(st.on == 1 && st.width == 320 && st.height == 240 && st.pictures >= compared && st.dropped == 0);
    CHECK(w.events >= 200 && w.on * 100 >= w.events * 98 && w.time_order && w.events <= st.events);
    CHECK(stats.dropped == 0 && w.allocations == 0);
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
}
#endif
}

int main() {
    std::puts("Neuromorphic event camera tests"); // stdout's buffer before the first probe (MSVC Debug counts the CRT's).
    TestClass();
    TestPackedRing();
    TestSdk();
#if defined(_WIN32)
    char fixture[1024]{};
    FixtureArgument(fixture, sizeof(fixture));
    TestWindow(fixture);
#endif
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "neuromorphic spike tests passed", failures);
    return failures ? 1 : 0;
}
