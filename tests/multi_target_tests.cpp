// Several objects at once (docs/picture.md, "Several objects at once"; docs/sdk.md, API 7), 2026-10-31.
// Portable (MSVC; GCC -O2 and ASan/UBSan):
// - (2026-11-01) objects that cross: two textured squares of different size and brightness on an X (B passing behind A) and
//   side by side (their edges a tile apart, one salient region): each keeps the id it had before the pass on every picture after
//   it, both ids are in the table on at least 80 % of the pictures while they are within two tiles of each other, and each
//   track's velocity (its line) is within 25 % of the truth (median vector error, apart); no allocation;
// - video::Perception on two obstacles approaching a still camera at once over a textured static background, at different
//   places, sizes and speeds (A: 3 m away at 1.0 m/s, half size 32 px at 3 m; B: 4 m away at 0.9 m/s, 45 px at 4 m: larger on
//   screen, later to arrive): from 0.4 s both in the object table on at least 85 % of the pictures, each keeping one track id
//   (its most frequent id on at least 90 % of the pictures it is found in, the two ids different), both approaches confirmed
//   at once on at least 40 % of them (an approach shows over time: the slower one's first second is too slow to measure), A
//   ranked first (the sooner time to contact) on at least 90 % of those, each one's time to contact within 20 % (median) of
//   the truth; objects off both obstacles on at most a tenth of the pictures; no allocation;
// - the SDK in live-sensor mode on the same scene with a stereo microphone pair (A's side a 500 Hz tone on the left channel, B's
//   a 1300 Hz tone on the right), the audio zoom on: "next target" starts tracking the most urgent object (A), the view and
//   the audio focus go to it, telemetry's target_track_id is A's id (MEDIA_OBJECT_TARGET on its entry); "next target" again
//   moves to B, "מטרה הבאה" back to A, media_engine_track_object(B's id) to B; the beam: A's tone over B's at least 6 dB more
//   while A is the target than while B is; no allocation while ingesting, stepping, pumping, querying and commanding;
//   "next target", "מטרה הבאה", "cible suivante" and the other languages parse to NextTarget, and "next one" stays NextMatch.
// Windows, with a hardware Direct3D 12 device (MEDIA_SENSOR_GPU): the GPU image quarter-turned (media_engine_set_orientation
// 0..3): a bright square of the camera picture lands where the turn puts it (within 3 pixels), the image reports its turns,
// an odd turn converts a region of the output's turned shape (360 x 480 of a 640 x 480 camera for a 640 x 480 image) and the
// image is covered to its corners.
#include "media/c_api.h"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/core/linear_memory_arena.hpp"
#include "media/video/Perception.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr unsigned kW = 640, kH = 480;
constexpr double kFps = 30, kPi = 3.14159265358979323846;
constexpr unsigned kPictures = 66;
std::uint32_t Hash(std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
double Noise(double u, double v, std::uint32_t seed) {
    const double fu = std::floor(u), fv = std::floor(v), a = u - fu, b = v - fv;
    const auto at = [&](int i, int j) { return static_cast<double>(Hash(static_cast<std::uint32_t>(i) * 7919u + static_cast<std::uint32_t>(j) * 104729u + seed) % 181u); };
    const int i = static_cast<int>(fu), j = static_cast<int>(fv);
    return (1 - a) * (1 - b) * at(i, j) + a * (1 - b) * at(i + 1, j) + (1 - a) * b * at(i, j + 1) + a * b * at(i + 1, j + 1);
}
// The two obstacles at picture f: centre (fractions), half size (pixels), time to contact (seconds).
struct Obstacle { double x, y, half, ttc; };
Obstacle A(unsigned f) { const double t = f / kFps, z = 3.0 - 1.0 * t; return {0.27, 0.40, 32 * 3.0 / z, z / 1.0}; }
Obstacle B(unsigned f) { const double t = f / kFps, z = 4.0 - 0.9 * t; return {0.73, 0.60, 45 * 4.0 / z, z / 0.9}; }
// The scene: a textured static background, each obstacle a square of its own texture (12 lattice cells across, so it scales
// with what carries it), 2 x 2 samples a pixel.
void Draw(unsigned f, std::uint8_t* luma) {
    const Obstacle o[2]{A(f), B(f)};
    for (unsigned y = 0; y < kH; ++y)
        for (unsigned x = 0; x < kW; ++x) {
            double sum = 0;
            for (int s = 0; s < 4; ++s) {
                const double px = x + 0.25 + 0.5 * (s & 1), py = y + 0.25 + 0.5 * (s >> 1);
                double v = 40 + Noise(px / 9.0, py / 9.0, 3);
                for (unsigned k = 0; k < 2; ++k) {
                    const double cx = o[k].x * kW, cy = o[k].y * kH, h = o[k].half;
                    if (std::fabs(px - cx) < h && std::fabs(py - cy) < h) v = 30 + Noise((px - cx) / h * 6 + 50 + 40 * k, (py - cy) / h * 6 + 50, 11 + k);
                }
                sum += v;
            }
            luma[y * kW + x] = static_cast<std::uint8_t>(std::clamp(std::lround(sum / 4), 0l, 255l));
        }
}
std::int64_t Pts(unsigned f) { return static_cast<std::int64_t>(std::llround(f * 1e7 / kFps)); }
// Which obstacle an object's centre lies on at picture f (0 A, 1 B, -1 neither).
int On(unsigned f, float x, float y) {
    const Obstacle o[2]{A(f), B(f)};
    for (int k = 0; k < 2; ++k)
        if (std::fabs(x * kW - o[k].x * kW) <= o[k].half + 8 && std::fabs(y * kH - o[k].y * kH) <= o[k].half + 8) return k;
    return -1;
}
// The id seen most often among `ids`, and how many times.
std::uint32_t Mode(const std::vector<std::uint32_t>& ids, unsigned& times) {
    std::uint32_t best = 0; times = 0;
    for (const auto id : ids) {
        const auto n = static_cast<unsigned>(std::count(ids.begin(), ids.end(), id));
        if (n > times) { times = n; best = id; }
    }
    return best;
}

std::vector<std::vector<std::uint8_t>> g_scene; // The pictures, drawn once.

void TestPerception(core::LinearMemoryArena& arena) {
    video::Perception p;
    CHECK(p.Configure(arena, kW, kH, 0));
    std::vector<std::uint32_t> ids[2];
    std::vector<double> errors[2];
    unsigned measured = 0, both = 0, confirmed_both = 0, a_first = 0, strays = 0;
    std::uint64_t allocations = 0; double push_us = 0;
    for (unsigned f = 0; f < kPictures; ++f) {
        allocation_probe::Begin();
        const auto began = std::chrono::steady_clock::now();
        const auto& r = p.Push(g_scene[f].data(), kW, Pts(f), {});
        push_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
        allocations += allocation_probe::End();
        if (f / kFps < 0.4) continue;
        ++measured;
        bool seen[2]{}; bool approaching[2]{}; int first = -1;
        for (unsigned i = 0; i < r.object_count; ++i) {
            const auto& o = r.objects[i];
            const int k = On(f, o.x, o.y);
            if (k < 0) { ++strays; continue; }
            if (seen[k]) continue; // A second object on the same obstacle: counted once (its first, the more urgent).
            seen[k] = true; ids[k].push_back(o.track_id);
            if (first < 0) first = k;
            if (o.approaching) {
                approaching[k] = true;
                const double truth = k == 0 ? A(f).ttc : B(f).ttc;
                errors[k].push_back(std::fabs(o.ttc_seconds - truth) / truth);
            }
        }
        if (seen[0] && seen[1]) ++both;
        if (approaching[0] && approaching[1]) { ++confirmed_both; if (first == 0) ++a_first; }
        if (f % 10 == 0 || f + 1 == kPictures) {
            std::printf("  picture %2u:", f);
            for (unsigned i = 0; i < r.object_count; ++i) {
                const auto& o = r.objects[i];
                std::printf(" [id %u %s (%.2f, %.2f) %.2fx%.2f ttc %.2f tiles %u support %u]", o.track_id, On(f, o.x, o.y) == 0 ? "A" : On(f, o.x, o.y) == 1 ? "B" : "?",
                            static_cast<double>(o.x), static_cast<double>(o.y), static_cast<double>(o.width), static_cast<double>(o.height), static_cast<double>(o.ttc_seconds), o.tiles, o.support);
            }
            std::printf("  (truth A %.2f s, B %.2f s)\n", A(f).ttc, B(f).ttc);
        }
    }
    unsigned times[2]{}; std::uint32_t id[2]{Mode(ids[0], times[0]), Mode(ids[1], times[1])};
    double median[2]{1, 1};
    for (unsigned k = 0; k < 2; ++k) if (!errors[k].empty()) { std::sort(errors[k].begin(), errors[k].end()); median[k] = errors[k][errors[k].size() / 2]; }
    std::printf("Perception, two approaching obstacles: both in the table on %u of %u pictures; A id %u on %u of %zu, B id %u on %u of %zu; "
                "both approaches confirmed on %u, A first on %u; TTC error median A %.1f %% (%zu), B %.1f %% (%zu); %u objects off both; "
                "%.0f us per picture; %llu allocations\n", both, measured, id[0], times[0], ids[0].size(), id[1], times[1], ids[1].size(), confirmed_both, a_first,
                100 * median[0], errors[0].size(), 100 * median[1], errors[1].size(), strays, push_us / kPictures, static_cast<unsigned long long>(allocations));
    CHECK(both * 100 >= measured * 85);
    CHECK(id[0] && id[1] && id[0] != id[1] && times[0] * 10 >= ids[0].size() * 9 && times[1] * 10 >= ids[1].size() * 9);
    CHECK(confirmed_both * 10 >= measured * 4 && a_first * 10 >= confirmed_both * 9);
    CHECK(median[0] <= 0.20 && median[1] <= 0.20 && strays * 10 <= measured);
    CHECK(allocations == 0);
}

// Objects that cross (2026-11-01): two textured squares moving over the same still background, `kCrossPictures` pictures at
// 30 per second. X: A (bright, half 30 px) from the upper left to the lower right, B (dark, half 22 px) from the upper right to
// the lower left, through the same point at the middle picture (A drawn over B: one hides the other). Side by side: A along
// y = 0.42 to the right, B along y = 0.58 to the left, their edges a tile apart as they pass (one salient region).
constexpr unsigned kCrossPictures = 72;
struct Mover { double x0, y0, x1, y1, half, base, gain; std::uint32_t seed; };
struct CrossScene { const char* name; Mover m[2]; };
constexpr CrossScene kCrossScenes[]{
    {"X crossing (A over B)", {{0.18, 0.28, 0.82, 0.72, 30, 150, 0.55, 21}, {0.82, 0.28, 0.18, 0.72, 22, 8, 0.25, 22}}},
    {"side by side", {{0.15, 0.42, 0.85, 0.42, 26, 150, 0.55, 23}, {0.85, 0.58, 0.15, 0.58, 26, 8, 0.25, 24}}},
};
void Centre(const Mover& m, unsigned f, double& x, double& y) {
    const double s = static_cast<double>(f) / (kCrossPictures - 1);
    x = m.x0 + (m.x1 - m.x0) * s; y = m.y0 + (m.y1 - m.y0) * s;
}
void DrawCrossing(const CrossScene& scene, unsigned f, std::uint8_t* luma) {
    double cx[2], cy[2];
    for (unsigned k = 0; k < 2; ++k) Centre(scene.m[k], f, cx[k], cy[k]);
    for (unsigned y = 0; y < kH; ++y)
        for (unsigned x = 0; x < kW; ++x) {
            double sum = 0;
            for (int s = 0; s < 4; ++s) {
                const double px = x + 0.25 + 0.5 * (s & 1), py = y + 0.25 + 0.5 * (s >> 1);
                double v = 40 + Noise(px / 9.0, py / 9.0, 3);
                for (int k = 1; k >= 0; --k) { // B first: A is drawn over it.
                    const auto& m = scene.m[k];
                    const double dx = px - cx[k] * kW, dy = py - cy[k] * kH;
                    if (std::fabs(dx) < m.half && std::fabs(dy) < m.half) v = m.base + m.gain * Noise(dx / 6 + 50, dy / 6 + 50, m.seed);
                }
                sum += v;
            }
            luma[y * kW + x] = static_cast<std::uint8_t>(std::clamp(std::lround(sum / 4), 0l, 255l));
        }
}
// Which square an object's centre is on at picture f: the nearer of those it lies on (-1: neither).
int OnMover(const CrossScene& scene, unsigned f, float x, float y) {
    int best = -1; double least = 0;
    for (int k = 0; k < 2; ++k) {
        double cx = 0, cy = 0; Centre(scene.m[k], f, cx, cy);
        const double dx = std::fabs(x * kW - cx * kW), dy = std::fabs(y * kH - cy * kH);
        if (dx > scene.m[k].half + 10 || dy > scene.m[k].half + 10) continue;
        if (best < 0 || std::hypot(dx, dy) < least) { best = k; least = std::hypot(dx, dy); }
    }
    return best;
}
void TestCrossing(core::LinearMemoryArena& arena) {
    std::vector<std::uint8_t> luma(kW * kH);
    for (const auto& scene : kCrossScenes) {
        CHECK(arena.Reset());
        video::Perception p;
        CHECK(p.Configure(arena, kW, kH, 0));
        // Before: until the squares' edges come within two tiles (80 px) of each other; after: once they are that far apart again.
        // The ids each square had last before, and every id each one shows after; during: pictures with both ids in the table.
        std::uint32_t before[2]{}; unsigned after_seen[2]{}, after_right[2]{}, after_pictures = 0, during = 0, during_both = 0, measured = 0;
        std::uint64_t allocations = 0;
        std::vector<double> speed_errors; // Apart (before and after): each square's object's velocity against the truth (relative).
        std::printf("Crossing, %s:\n", scene.name);
        for (unsigned f = 0; f < kCrossPictures; ++f) {
            DrawCrossing(scene, f, luma.data());
            allocation_probe::Begin();
            const auto& r = p.Push(luma.data(), kW, Pts(f), {});
            allocations += allocation_probe::End();
            double ax = 0, ay = 0, bx = 0, by = 0; Centre(scene.m[0], f, ax, ay); Centre(scene.m[1], f, bx, by);
            const double gap = (std::max)(std::fabs(ax - bx) * kW, std::fabs(ay - by) * kH) - scene.m[0].half - scene.m[1].half;
            const int phase = gap > 80 ? (f < kCrossPictures / 2 ? 0 : 2) : 1;
            if (f < 6) continue; // Saliency and the tracks settle.
            ++measured;
            std::uint32_t id_on[2]{};
            for (unsigned i = 0; i < r.object_count; ++i) {
                const auto& o = r.objects[i];
                const int k = OnMover(scene, f, o.x, o.y);
                if (k < 0 || id_on[k]) continue;
                id_on[k] = o.track_id;
                if (phase != 1 && o.age > 8) { // A track's line once it has a path.
                    const auto& m = scene.m[k]; const double seconds = (kCrossPictures - 1) / kFps;
                    const double tx = (m.x1 - m.x0) / seconds * kW, ty = (m.y1 - m.y0) / seconds * kH; // Pixels per second.
                    speed_errors.push_back(std::hypot(o.vx * kW - tx, o.vy * kH - ty) / std::hypot(tx, ty));
                }
            }
            if (phase == 0) for (unsigned k = 0; k < 2; ++k) if (id_on[k]) before[k] = id_on[k];
            if (phase == 1) {
                ++during;
                bool has[2]{};
                for (unsigned i = 0; i < r.object_count; ++i) for (unsigned k = 0; k < 2; ++k) has[k] = has[k] || (before[k] && r.objects[i].track_id == before[k]);
                if (has[0] && has[1]) ++during_both;
            }
            if (phase == 2) {
                ++after_pictures;
                for (unsigned k = 0; k < 2; ++k) if (id_on[k]) { ++after_seen[k]; if (id_on[k] == before[k]) ++after_right[k]; }
            }
            if (f % 6 == 0 || (phase == 1 && f % 2 == 0)) {
                std::printf("  picture %2u (%s, gap %4.0f px):", f, phase == 0 ? "before" : phase == 1 ? "during" : "after", gap);
                for (unsigned i = 0; i < r.object_count; ++i) {
                    const auto& o = r.objects[i]; const int k = OnMover(scene, f, o.x, o.y);
                    std::printf(" [id %u %s (%.2f, %.2f) v (%+.2f, %+.2f) tiles %u]", o.track_id, k == 0 ? "A" : k == 1 ? "B" : "?", static_cast<double>(o.x), static_cast<double>(o.y),
                                static_cast<double>(o.vx), static_cast<double>(o.vy), o.tiles);
                }
                std::printf("\n");
            }
        }
        std::sort(speed_errors.begin(), speed_errors.end());
        const double speed_error = speed_errors.empty() ? 1.0 : speed_errors[speed_errors.size() / 2];
        std::printf("Crossing, %s: ids before A %u, B %u; during: both ids in the table on %u of %u pictures; after: A found on %u of %u pictures, "
                    "its id kept on %u; B found on %u, kept on %u; velocity error (vector, apart) median %.0f %% (%zu); %llu allocations\n", scene.name, before[0],
                    before[1], during_both, during, after_seen[0], after_pictures, after_right[0], after_seen[1], after_right[1], 100 * speed_error, speed_errors.size(),
                    static_cast<unsigned long long>(allocations));
        CHECK(before[0] && before[1] && before[0] != before[1] && during > 4 && after_pictures > 4);
        CHECK(speed_errors.size() > 40 && speed_error <= 0.25);
        CHECK(during_both * 10 >= during * 8);
        for (unsigned k = 0; k < 2; ++k) CHECK(after_seen[k] * 10 >= after_pictures * 8 && after_right[k] == after_seen[k]);
        CHECK(allocations == 0 && measured > 0);
    }
}

// The SDK in live-sensor mode.
constexpr std::size_t kStorage = 48u << 20;
std::vector<unsigned char> g_storage;
void* Aligned(unsigned char* base) {
    const std::size_t a = media_engine_storage_alignment();
    return base + (a - reinterpret_cast<std::uintptr_t>(base) % a) % a;
}
// Tones' power in the pumped output: the DFT at each tone's frequency over a window, both channels.
struct Tones {
    double re[2][2]{}, im[2][2]{}; // [tone][channel]
    unsigned n = 0;
    void Add(const float* frames, unsigned count, std::uint64_t first) {
        static constexpr double kHz[2]{500.0, 1300.0};
        for (unsigned i = 0; i < count; ++i)
            for (unsigned t = 0; t < 2; ++t) {
                const double w = 2 * kPi * kHz[t] * static_cast<double>(first + i) / 48000.0, c = std::cos(w), s = std::sin(w);
                for (unsigned ch = 0; ch < 2; ++ch) { re[t][ch] += frames[2 * i + ch] * c; im[t][ch] -= frames[2 * i + ch] * s; }
            }
        n += count;
    }
    // Decibels of the 500 Hz tone (A's side) over the 1300 Hz one (B's).
    [[nodiscard]] double Ratio() const {
        double p[2]{};
        for (unsigned t = 0; t < 2; ++t) for (unsigned ch = 0; ch < 2; ++ch) p[t] += re[t][ch] * re[t][ch] + im[t][ch] * im[t][ch];
        return 10 * std::log10((p[0] + 1e-30) / (p[1] + 1e-30));
    }
};
void TestSdk() {
    g_storage.assign(kStorage, 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_rate = 48000; config.audio_channels = 2;
    config.modules = MEDIA_MODULE_TELEMETRY;
    media_engine_t* e = nullptr;
    CHECK(media_engine_create(&config, Aligned(g_storage.data()), kStorage - 4096, &e) == MEDIA_OK);
    if (!e) return;
    media_sensor_config_t sensor; media_sensor_config_init(&sensor);
    sensor.width = kW; sensor.height = kH; sensor.audio_rate = 48000; sensor.audio_channels = 2;
    if (media_engine_open_sensor(e, &sensor) != MEDIA_OK) { std::printf("FAIL sensor open: stage %u\n", media_engine_last_error(e, nullptr)); ++failures; media_engine_destroy(e); return; }
    static std::uint8_t uv[kW * kH / 2];
    std::memset(uv, 128, sizeof(uv));
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = kW; frame.height = kH;
    frame.planes[1] = uv; frame.strides[0] = frame.strides[1] = kW;
    static float mic[480 * 2], out[480 * 2];
    media_frame_telemetry_t t{};
    std::uint64_t allocations = 0, pumped = 0;
    unsigned next = 0;
    // The ids of the objects on A and B (from the table), each window's target and view, the beam's tones per window.
    std::uint32_t id_on[2]{};
    struct Window { const char* what = ""; unsigned from = 0, to = 0; int target = 0; unsigned right = 0, frames = 0; float view_x = 0; bool flagged = true; Tones tones; };
    Window windows[4];
    static constexpr const char* kWhat[4]{"next target -> A", "next target -> B", "\xd7\x9e\xd7\x98\xd7\xa8\xd7\x94 \xd7\x94\xd7\x91\xd7\x90\xd7\x94 -> A", "track_object(B) -> B"};
    static constexpr unsigned kFrom[4]{27, 43, 57, 63}, kTo[4]{35, 51, 61, 66};
    for (unsigned i = 0; i < 4; ++i) { windows[i].what = kWhat[i]; windows[i].from = kFrom[i]; windows[i].to = kTo[i]; windows[i].target = static_cast<int>(i % 2); }
    int results[4]{-1, -1, -1, -1}; int zoom = -1;
    for (unsigned step = 0; next < kPictures; ++step) {
        const std::int64_t now = static_cast<std::int64_t>(step) * 100000;
        const unsigned due = (step * 30 + 99) / 100;
        for (unsigned k = 0; k < 480; ++k) {
            const double n = static_cast<double>(step) * 480 + k;
            mic[2 * k] = static_cast<float>(0.2 * std::sin(2 * kPi * 500 * n / 48000)); mic[2 * k + 1] = static_cast<float>(0.2 * std::sin(2 * kPi * 1300 * n / 48000));
        }
        allocation_probe::Begin();
        CHECK(media_engine_ingest_audio_samples(e, mic, MEDIA_SAMPLE_F32, 480, now) == MEDIA_OK);
        (void)media_engine_pump_audio(e, out, 480, now);
        allocations += allocation_probe::End();
        for (auto& w : windows) if (next >= w.from && next < w.to) w.tones.Add(out, 480, pumped);
        pumped += 480;
        while (next < due && next < kPictures) {
            const unsigned f = next++;
            frame.planes[0] = g_scene[f].data(); frame.timestamp_100ns = Pts(f);
            allocation_probe::Begin();
            CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
            (void)media_engine_step(e, now);
            t.struct_size = sizeof(t);
            (void)media_query_frame_telemetry(e, &t);
            // The commands, between pictures, as a host gives them.
            if (f == 12) zoom = media_engine_command(e, "audio zoom");
            if (f == 21) results[0] = media_engine_command(e, "next target");
            if (f == 37) results[1] = media_engine_command(e, "next target");
            if (f == 52) results[2] = media_engine_command(e, "\xd7\x9e\xd7\x98\xd7\xa8\xd7\x94 \xd7\x94\xd7\x91\xd7\x90\xd7\x94"); // מטרה הבאה
            if (f == 61) results[3] = media_engine_track_object(e, id_on[1]);
            allocations += allocation_probe::End();
            for (unsigned i = 0; i < t.object_count; ++i) { const int k = On(f, t.objects[i].x, t.objects[i].y); if (k >= 0) id_on[k] = t.objects[i].track_id; }
            for (auto& w : windows) {
                if (f < w.from || f >= w.to) continue;
                ++w.frames; w.view_x = t.view_x;
                if (t.target_track_id && t.target_track_id == id_on[w.target]) ++w.right;
                for (unsigned i = 0; i < t.object_count; ++i)
                    if (t.objects[i].track_id == t.target_track_id) w.flagged = w.flagged && (t.objects[i].flags & MEDIA_OBJECT_TARGET) != 0;
            }
        }
    }
    std::printf("SDK: \"audio zoom\" %d; commands %d %d %d %d (0: done); ids A %u, B %u; %llu allocations\n", zoom, results[0], results[1], results[2], results[3],
                id_on[0], id_on[1], static_cast<unsigned long long>(allocations));
    CHECK(zoom == MEDIA_OK && results[0] == MEDIA_OK && results[1] == MEDIA_OK && results[2] == MEDIA_OK && results[3] == MEDIA_OK);
    double ratio[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto& w = windows[i];
        ratio[i] = w.tones.Ratio();
        const double truth_x = w.target == 0 ? 0.27 : 0.73;
        std::printf("  %s: target right on %u of %u pictures, view x %.3f (the object at %.2f), MEDIA_OBJECT_TARGET %s; A's tone over B's %+.1f dB\n", w.what, w.right, w.frames,
                    static_cast<double>(w.view_x), truth_x, w.flagged ? "set" : "MISSING", ratio[i]);
        CHECK(w.frames && w.right == w.frames && std::fabs(w.view_x - truth_x) <= 0.08 && w.flagged);
    }
    std::printf("SDK: the beam, A's tone over B's: A targeted %+.1f / %+.1f dB, B targeted %+.1f / %+.1f dB\n", ratio[0], ratio[2], ratio[1], ratio[3]);
    CHECK(ratio[0] - ratio[1] >= 6 && ratio[2] - ratio[1] >= 6);
    CHECK(allocations == 0);
    media_engine_destroy(e);
}
// T, then "next target" on the very next picture (2026-11-01): the view is put on the table's first object (track_object, then
// "stop tracking": the view stays there), "track the target" locks there, and the target is that object at once (telemetry's
// target_track_id right after the command); "next target" on the next picture moves to the other object at once (the target id
// and the view), and the tracker follows it from the picture after (MEDIA_OBJECT_TARGET on its entry 6 pictures later).
void TestTrackThenNext() {
    g_storage.assign(kStorage, 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY;
    media_engine_t* e = nullptr;
    CHECK(media_engine_create(&config, Aligned(g_storage.data()), kStorage - 4096, &e) == MEDIA_OK);
    if (!e) return;
    media_sensor_config_t sensor; media_sensor_config_init(&sensor);
    sensor.width = kW; sensor.height = kH;
    CHECK(media_engine_open_sensor(e, &sensor) == MEDIA_OK);
    static std::uint8_t uv[kW * kH / 2];
    std::memset(uv, 128, sizeof(uv));
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = kW; frame.height = kH;
    frame.planes[1] = uv; frame.strides[0] = frame.strides[1] = kW;
    media_frame_telemetry_t t{}; t.struct_size = sizeof(t);
    std::uint32_t first = 0, other = 0, after_track = 0, after_next = 0, followed = 0; int results[4]{-1, -1, -1, -1};
    float view_after_next = -1, other_x = -1; bool flagged = false;
    std::uint64_t allocations = 0;
    for (unsigned f = 0; f < 40; ++f) {
        frame.planes[0] = g_scene[f].data(); frame.timestamp_100ns = Pts(f);
        allocation_probe::Begin();
        CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
        (void)media_engine_step(e, Pts(f));
        (void)media_query_frame_telemetry(e, &t);
        if (f == 17 && t.object_count >= 2) first = t.objects[0].track_id;
        if (f == 18) results[0] = media_engine_track_object(e, first);
        if (f == 22) results[1] = media_engine_command(e, "stop tracking");
        if (f == 26) { results[2] = media_engine_command(e, "track the target"); (void)media_query_frame_telemetry(e, &t); after_track = t.target_track_id; }
        if (f == 27) {
            results[3] = media_engine_command(e, "next target"); (void)media_query_frame_telemetry(e, &t);
            after_next = t.target_track_id; view_after_next = t.view_x;
            for (unsigned i = 0; i < t.object_count; ++i) if (t.objects[i].track_id != first) { other = t.objects[i].track_id; other_x = t.objects[i].x; }
        }
        allocations += allocation_probe::End();
        if (f == 33) {
            followed = t.target_track_id;
            for (unsigned i = 0; i < t.object_count; ++i) if (t.objects[i].track_id == followed) flagged = (t.objects[i].flags & MEDIA_OBJECT_TARGET) != 0;
        }
    }
    std::printf("T then next target: first object %u; track_object %d, \"stop tracking\" %d, \"track the target\" %d -> target %u at once; \"next target\" on the next "
                "picture %d -> target %u (the other object %u at x %.2f), view x %.3f; 6 pictures later following %u (flag %s); %llu allocations\n", first, results[0],
                results[1], results[2], after_track, results[3], after_next, other, static_cast<double>(other_x), static_cast<double>(view_after_next), followed,
                flagged ? "set" : "MISSING", static_cast<unsigned long long>(allocations));
    CHECK(first && results[0] == MEDIA_OK && results[1] == MEDIA_OK && results[2] == MEDIA_OK && results[3] == MEDIA_OK);
    CHECK(after_track == first && other && after_next == other && std::fabs(view_after_next - other_x) <= 0.08f && followed == other && flagged);
    CHECK(allocations == 0);
    media_engine_destroy(e);
}
void TestCommands() {
    using ai::IntentKind;
    ai::MediaBounds film; film.duration_100ns = 600000000; film.seekable = true;
    const char* next[]{"next target", "switch target", "the other target", "follow the next object", "\xd7\x9e\xd7\x98\xd7\xa8\xd7\x94 \xd7\x94\xd7\x91\xd7\x90\xd7\x94",
                       "\xd7\x94\xd7\x97\xd7\x9c\xd7\xa3 \xd7\x9e\xd7\x98\xd7\xa8\xd7\x94", "cible suivante", "changer de cible", "siguiente objetivo", "n\xc3\xa4" "chstes Ziel",
                       "\xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd1\x83\xd1\x8e\xd1\x89\xd0\xb0\xd1\x8f \xd1\x86\xd0\xb5\xd0\xbb\xd1\x8c", "\xd8\xa7\xd9\x84\xd9\x87\xd8\xaf\xd9\x81 \xd8\xa7\xd9\x84\xd8\xaa\xd8\xa7\xd9\x84\xd9\x8a",
                       "\xe4\xb8\x8b\xe4\xb8\x80\xe4\xb8\xaa\xe7\x9b\xae\xe6\xa0\x87", "\xe6\xac\xa1\xe3\x81\xae\xe3\x82\xbf\xe3\x83\xbc\xe3\x82\xb2\xe3\x83\x83\xe3\x83\x88"};
    unsigned right = 0;
    for (const char* text : next) {
        const auto i = ai::LocalIntentDispatcher::Parse(text, film);
        if (i.status == ai::IntentStatus::Ok && i.kind == IntentKind::NextTarget) ++right; else std::printf("  \"%s\" -> %s %s\n", text, ai::Name(i.kind), ai::Name(i.status));
    }
    const bool neighbours = ai::LocalIntentDispatcher::Parse("next one", film).kind != IntentKind::NextTarget &&
                            ai::LocalIntentDispatcher::Parse("track the target", film).kind == IntentKind::TrackTarget &&
                            ai::LocalIntentDispatcher::Parse("next chapter", film).kind == IntentKind::NextChapter;
    std::printf("Commands: %u of %zu parse to NextTarget; \"next one\", \"track the target\", \"next chapter\" keep theirs: %s\n", right, sizeof(next) / sizeof(next[0]), neighbours ? "yes" : "NO");
    CHECK(right == sizeof(next) / sizeof(next[0]) && neighbours);
}

#if defined(_WIN32)
// The GPU image quarter-turned: a 640 x 480 camera (dark, a bright 40 x 40 square centred at (220, 120)) into a 640 x 480 image.
void TestGpuTurns() {
    std::vector<unsigned char> storage(media_engine_storage_bytes() + media_engine_storage_alignment());
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = 0;
    media_engine_t* e = nullptr;
    if (media_engine_create(&config, Aligned(storage.data()), media_engine_storage_bytes(), &e) != MEDIA_OK) { CHECK(false); return; }
    media_sensor_config_t s; media_sensor_config_init(&s);
    s.width = kW; s.height = kH; s.options = MEDIA_SENSOR_GPU; s.output_width = kW; s.output_height = kH;
    const auto status = media_engine_open_sensor(e, &s);
    if (status == MEDIA_ERROR_UNSUPPORTED) { std::puts("GPU quarter turns: no hardware Direct3D 12 device, skipped"); media_engine_destroy(e); return; }
    CHECK(status == MEDIA_OK);
    static std::uint8_t y[kW * kH], uv[kW * kH / 2];
    std::memset(y, 40, sizeof(y)); std::memset(uv, 128, sizeof(uv));
    for (unsigned j = 100; j < 140; ++j) for (unsigned i = 200; i < 240; ++i) y[j * kW + i] = 220;
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = kW; frame.height = kH;
    frame.planes[0] = y; frame.planes[1] = uv; frame.strides[0] = frame.strides[1] = kW;
    unsigned frames = 0, converted = 0;
    const auto push = [&](media_gpu_image_t& image) {
        frame.timestamp_100ns = Pts(frames++);
        if (media_engine_ingest_video_frame(e, &frame) != MEDIA_OK) return false;
        ++converted; image.struct_size = sizeof(image);
        for (const auto start = GetTickCount64(); GetTickCount64() - start < 3000;) {
            if (media_query_gpu_image(e, &image) == MEDIA_OK && image.sequence >= converted) return true;
            SwitchToThread();
        }
        return false;
    };
    media_gpu_image_t image{};
    CHECK(push(image));
    unsigned right = 0;
    for (unsigned turns = 0; turns < 4; ++turns) {
        CHECK(media_engine_set_orientation(e, turns, 0) == MEDIA_OK);
        bool ok = push(image) && push(image);
        // The square's centre where the turn puts it: display = D stored about the centres, the region (the whole camera, or
        // 360 x 480 of it across) scaled to the image.
        static constexpr int kD[4][4]{{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
        const double sx = 220 - kW / 2.0, sy = 120 - kH / 2.0, scale = turns % 2 ? 480.0 / 640.0 : 1.0;
        const double ex = kW / 2.0 + (kD[turns][0] * sx + kD[turns][1] * sy) / scale, ey = kH / 2.0 + (kD[turns][2] * sx + kD[turns][3] * sy) / scale;
        double cx = 0, cy = 0, n = 0, corner = 255;
        if (ok) {
            for (unsigned j = 0; j < image.height; ++j)
                for (unsigned i = 0; i < image.width; ++i) {
                    const auto* p = image.rgba + static_cast<std::size_t>(j) * image.stride + 4 * i;
                    if (p[1] > 128) { cx += i + 0.5; cy += j + 0.5; ++n; }
                }
            for (const unsigned c : {0u, 1u, 2u, 3u}) {
                const unsigned i = c & 1 ? image.width - 1 : 0, j = c & 2 ? image.height - 1 : 0;
                corner = (std::min)(corner, static_cast<double>(image.rgba[static_cast<std::size_t>(j) * image.stride + 4 * i + 1]));
            }
        }
        if (n) { cx /= n; cy /= n; }
        const unsigned rw = image.right - image.left, rh = image.bottom - image.top;
        const bool region = turns % 2 ? rw == 360 && rh == 480 : rw == kW && rh == kH;
        const bool good = ok && image.quarter_turns == turns && !image.mirrored && n > 0 && std::fabs(cx - ex) <= 3 && std::fabs(cy - ey) <= 3 && region && corner >= 15;
        std::printf("GPU image, %u quarter turn(s): reports %u; square at (%.1f, %.1f), expected (%.1f, %.1f); region %ux%u; darkest corner %.0f%s\n", turns, image.quarter_turns,
                    cx, cy, ex, ey, rw, rh, corner, good ? "" : "  <- WRONG");
        right += good ? 1u : 0u;
    }
    CHECK(right == 4);
    media_engine_destroy(e);
}
// MEDIA_SENSOR_GPU_FIT (2026-11-01): the same 640 x 480 camera into a 640 x 480 image, with a bright marker in each corner of the
// camera's picture (squares of 24, 16, 32 and 20 samples: top left, top right, bottom right, bottom left). At each quarter turn
// every marker is in the image where the turn puts it (its centre within 3 pixels, its size the scale's), so no corner of the
// camera is lost; at 90 and 270 degrees the image reports MEDIA_GPU_FITTED and the picture 360 x 480 in the middle
// (picture_left 140 .. picture_right 500), the bars are black exactly (every byte 0) and the picture's edge columns are not; at
// 0 and 180 degrees nothing is barred.
void TestGpuFit() {
    std::vector<unsigned char> storage(media_engine_storage_bytes() + media_engine_storage_alignment());
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = 0;
    media_engine_t* e = nullptr;
    if (media_engine_create(&config, Aligned(storage.data()), media_engine_storage_bytes(), &e) != MEDIA_OK) { CHECK(false); return; }
    media_sensor_config_t s; media_sensor_config_init(&s);
    s.width = kW; s.height = kH; s.options = MEDIA_SENSOR_GPU | MEDIA_SENSOR_GPU_FIT; s.output_width = kW; s.output_height = kH;
    const auto status = media_engine_open_sensor(e, &s);
    if (status == MEDIA_ERROR_UNSUPPORTED) { std::puts("GPU fit: no hardware Direct3D 12 device, skipped"); media_engine_destroy(e); return; }
    CHECK(status == MEDIA_OK);
    static std::uint8_t y[kW * kH], uv[kW * kH / 2];
    std::memset(y, 40, sizeof(y)); std::memset(uv, 128, sizeof(uv));
    static constexpr unsigned kSize[4]{24, 16, 32, 20};
    // Each marker's centre in the camera (samples): top left, top right, bottom right, bottom left.
    double mx[4], my[4];
    for (unsigned c = 0; c < 4; ++c) {
        const unsigned n = kSize[c], x0 = (c == 1 || c == 2) ? kW - n : 0, y0 = c >= 2 ? kH - n : 0;
        for (unsigned j = y0; j < y0 + n; ++j) for (unsigned i = x0; i < x0 + n; ++i) y[j * kW + i] = 220;
        mx[c] = x0 + n / 2.0; my[c] = y0 + n / 2.0;
    }
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = kW; frame.height = kH;
    frame.planes[0] = y; frame.planes[1] = uv; frame.strides[0] = frame.strides[1] = kW;
    unsigned frames = 0, converted = 0;
    const auto push = [&](media_gpu_image_t& image) {
        frame.timestamp_100ns = Pts(frames++);
        if (media_engine_ingest_video_frame(e, &frame) != MEDIA_OK) return false;
        ++converted; image.struct_size = sizeof(image);
        for (const auto start = GetTickCount64(); GetTickCount64() - start < 3000;) {
            if (media_query_gpu_image(e, &image) == MEDIA_OK && image.sequence >= converted) return true;
            SwitchToThread();
        }
        return false;
    };
    media_gpu_image_t image{};
    CHECK(push(image));
    unsigned right = 0;
    for (unsigned turns = 0; turns < 4; ++turns) {
        CHECK(media_engine_set_orientation(e, turns, 0) == MEDIA_OK);
        const bool ok = push(image) && push(image);
        static constexpr int kD[4][4]{{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
        const double scale = turns % 2 ? 640.0 / 480.0 : 1.0; // Camera samples per image pixel: the whole 640 across 480 rows.
        bool markers = ok;
        double worst = 0;
        for (unsigned c = 0; c < 4 && ok; ++c) {
            const double sx = mx[c] - kW / 2.0, sy = my[c] - kH / 2.0;
            const double ex = kW / 2.0 + (kD[turns][0] * sx + kD[turns][1] * sy) / scale, ey = kH / 2.0 + (kD[turns][2] * sx + kD[turns][3] * sy) / scale;
            double cx = 0, cy = 0, n = 0;
            for (int j = static_cast<int>(ey) - 30; j <= static_cast<int>(ey) + 30; ++j)
                for (int i = static_cast<int>(ex) - 30; i <= static_cast<int>(ex) + 30; ++i) {
                    if (i < 0 || j < 0 || i >= static_cast<int>(image.width) || j >= static_cast<int>(image.height)) continue;
                    const auto* p = image.rgba + static_cast<std::size_t>(j) * image.stride + 4 * static_cast<std::size_t>(i);
                    if (p[1] > 128) { cx += i + 0.5; cy += j + 0.5; ++n; }
                }
            const double area = (kSize[c] / scale) * (kSize[c] / scale);
            if (n) { cx /= n; cy /= n; }
            const double off = n ? std::hypot(cx - ex, cy - ey) : 99;
            worst = (std::max)(worst, off);
            markers = markers && n > 0.75 * area && n < 1.25 * area && off <= 3;
        }
        // The bars: every byte of the columns outside the picture (two columns' margin) 0; the picture's edge columns not black.
        const bool odd = turns % 2 != 0;
        const unsigned left = odd ? 140u : 0u, rightx = odd ? 500u : kW;
        bool bars_black = true, edges_lit = true;
        for (unsigned j = 0; ok && j < image.height; ++j) {
            const auto* row = image.rgba + static_cast<std::size_t>(j) * image.stride;
            for (unsigned i = 0; odd && i < image.width; ++i)
                if ((i + 2 < left || i >= rightx + 2) && (row[4 * i] | row[4 * i + 1] | row[4 * i + 2])) bars_black = false;
            if (row[4 * (left + 1) + 1] < 15 || row[4 * (rightx - 2) + 1] < 15) edges_lit = false;
        }
        const bool flagged = (image.flags & MEDIA_GPU_FITTED) != 0;
        const bool rectangle = odd ? flagged && image.picture_left == 140 && image.picture_right == 500 && image.picture_top == 0 && image.picture_bottom == kH
                                   : !flagged && image.picture_left == 0 && image.picture_right == kW && image.picture_top == 0 && image.picture_bottom == kH;
        const unsigned rw = image.right - image.left, rh = image.bottom - image.top;
        const bool good = ok && image.quarter_turns == turns && markers && bars_black && edges_lit && rectangle && rw == kW && rh == kH;
        std::printf("GPU fit, %u quarter turn(s): region %ux%u (the whole camera), %s, picture %u..%u x %u..%u; four corner markers %s (worst %.1f px); "
                    "bars %s, the picture's edges %s%s\n", turns, rw, rh, flagged ? "FITTED" : "not barred", image.picture_left, image.picture_right, image.picture_top,
                    image.picture_bottom, markers ? "in place" : "MISSING OR MISPLACED", worst, odd ? (bars_black ? "black" : "NOT BLACK") : "none",
                    edges_lit ? "lit" : "BLACK", good ? "" : "  <- WRONG");
        right += good ? 1u : 0u;
    }
    CHECK(right == 4);
    // API 7's struct is still answered (through reserved).
    media_gpu_image_t old{}; old.struct_size = offsetof(media_gpu_image_t, picture_left);
    CHECK(media_query_gpu_image(e, &old) == MEDIA_OK && old.width == kW && old.picture_right == 0);
    media_engine_destroy(e);
}
#endif
}

int main() {
    g_scene.resize(kPictures, std::vector<std::uint8_t>(kW * kH));
    for (unsigned f = 0; f < kPictures; ++f) Draw(f, g_scene[f].data());
    core::LinearMemoryArena arena;
    if (!arena.Initialize(8u << 20)) { std::printf("FAIL: arena\n"); return 1; }
    TestPerception(arena);
    TestCrossing(arena);
    TestSdk();
    TestTrackThenNext();
    TestCommands();
#if defined(_WIN32)
    TestGpuTurns();
    TestGpuFit();
#endif
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "multi-target tests passed", failures);
    return failures ? 1 : 0;
}
