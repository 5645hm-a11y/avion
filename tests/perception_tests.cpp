// Physical-world perception (docs/picture.md, "Perception"; docs/audio-dsp.md, "Sound localisation and the audio saccade"):
// video::Perception and audio::SoundLocator on synthetic scenes whose truth is known. Portable; MSVC and GCC.
// - an obstacle approaching at constant speed (its picture grows as Z0 / Z): looming flagged, the time to contact against the
//   truth (Z / v) and falling, the hazard on the obstacle;
// - the same texture translating over a textured static background, and a pan: no looming; each tile's flow against the truth;
// - a camera pan with the IMU's yaw: the dominant motion is the scene's own (0), without the IMU it is the pan;
// - saliency: a small moving object on a static textured scene is the mask, a static scene leaves it empty;
// - camera shake with the IMU: no looming, no motion, the object salient and next to nothing else; the stabiliser holds the view;
// - the sound locator: a one-sided pulse (right, left), a panned pulse (constant-power law), a spaced pair's time difference,
//   noise below the onset, the refractory time;
// - "look at sound", "עקוב אחרי הקול" and the other languages, and their neighbours;
// - no allocation in Push or Process;
// - the search's SSE2 costs (2026-10-28) against the scalar loop: the approach, the translation and the shake run both ways,
//   every picture's result and tile flow equal bit for bit; the time per 640x480 picture of each (under 0.6 ms vectorised in
//   an optimised x86-64 build, the fully textured pan too since the predicted diamond search, 2026-10-29).
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/audio/SoundLocator.hpp"
#include "media/video/Perception.hpp"
#include "media/video/Stabilizer.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kW = 640, kH = 480;
constexpr double kFps = 30;
std::uint32_t Hash(std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
// Value noise: hashed lattice values, bilinear between them (a texture that scales with what carries it).
double Noise(double u, double v, std::uint32_t seed) {
    const double fu = std::floor(u), fv = std::floor(v), a = u - fu, b = v - fv;
    const auto at = [&](int i, int j) { return static_cast<double>(Hash(static_cast<std::uint32_t>(i) * 7919u + static_cast<std::uint32_t>(j) * 104729u + seed) % 181u); };
    const int i = static_cast<int>(fu), j = static_cast<int>(fv);
    return (1 - a) * (1 - b) * at(i, j) + a * (1 - b) * at(i + 1, j) + (1 - a) * b * at(i, j + 1) + a * b * at(i + 1, j + 1);
}
struct Picture {
    std::vector<std::uint8_t> luma = std::vector<std::uint8_t>(kW * kH);
    // background: 0 flat grey, 1 textured (static in the world; `pan` samples it shifted). Object: centre (cx, cy), half size,
    // its texture in its own coordinates (12 lattice cells across). 2x2 samples per pixel.
    void Draw(int background, double pan, bool object, double cx, double cy, double half) {
        for (unsigned y = 0; y < kH; ++y)
            for (unsigned x = 0; x < kW; ++x) {
                double sum = 0;
                for (int s = 0; s < 4; ++s) {
                    const double px = x + 0.25 + 0.5 * (s & 1), py = y + 0.25 + 0.5 * (s >> 1);
                    double v = background ? 40 + Noise((px + pan) / 9.0, py / 9.0, 3) : 128;
                    if (object && std::fabs(px - cx) < half && std::fabs(py - cy) < half)
                        v = 30 + Noise((px - cx) / half * 6 + 50, (py - cy) / half * 6 + 50, 11);
                    sum += v;
                }
                luma[y * kW + x] = static_cast<std::uint8_t>(std::clamp(std::lround(sum / 4), 0l, 255l));
            }
    }
};
std::int64_t Pts(unsigned frame) { return static_cast<std::int64_t>(std::llround(frame * 1e7 / kFps)); }

void TestLooming(core::LinearMemoryArena& arena) {
    video::Perception p;
    CHECK(p.Configure(arena, kW, kH, 0));
    // A square obstacle 3 m away coming at 1 m/s: its half size 50 px at 3 m, 50 Z0 / Z; the truth TTC = Z / v.
    Picture pic; unsigned flagged = 0, measured = 0; std::vector<double> errors; double early = 0, late = 0; unsigned early_n = 0, late_n = 0;
    std::vector<double> hazards; unsigned outside = 0; std::uint64_t allocations = 0; double push_us = 0; unsigned pushes = 0; unsigned grounded = 0;
    for (unsigned f = 0; f <= 60; ++f) {
        const double t = f / kFps, z = 3.0 - t, half = 50 * 3.0 / z, truth = z;
        pic.Draw(0, 0, true, 0.55 * kW, 0.45 * kH, half);
        allocation_probe::Begin();
        const auto began = std::chrono::steady_clock::now();
        const auto& r = p.Push(pic.luma.data(), kW, Pts(f), {});
        push_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count(); ++pushes;
        allocations += allocation_probe::End();
        grounded += r.ground ? 1u : 0u; // A still camera travels over no ground (2026-10-28: the obstacle's lower half was taken for one).
        if (t < 0.2) continue;
        ++measured;
        if (!r.looming) continue;
        ++flagged; errors.push_back(std::fabs(r.ttc_seconds - truth) / truth);
        hazards.push_back(std::hypot(r.hazard_x - 0.55, r.hazard_y - 0.45));
        if (std::fabs(r.hazard_x - 0.55) * kW > half || std::fabs(r.hazard_y - 0.45) * kH > half) ++outside; // Off the obstacle.
        if (t < 0.7) { early += r.ttc_seconds; ++early_n; }
        if (t > 1.5) { late += r.ttc_seconds; ++late_n; }
    }
    std::sort(errors.begin(), errors.end()); std::sort(hazards.begin(), hazards.end());
    const double median = errors.empty() ? 1 : errors[errors.size() / 2], p90 = errors.empty() ? 1 : errors[errors.size() * 9 / 10], worst = errors.empty() ? 1 : errors.back();
    const double hazard = hazards.empty() ? 1 : hazards[hazards.size() / 2], hazard_worst = hazards.empty() ? 1 : hazards.back();
    early = early_n ? early / early_n : 0; late = late_n ? late / late_n : 0;
    std::printf("Looming: flagged on %u of %u pictures; TTC error median %.1f %%, 90th percentile %.1f %%, worst %.1f %% (the first pictures: a slow expansion over few tiles); "
                "TTC %.2f s early (truth 2.55) -> %.2f s late (truth 1.25); hazard %.3f from the obstacle's centre (median; worst %.3f), off the obstacle %u times; %.0f us per 640x480 picture; %llu allocations\n",
                flagged, measured, 100 * median, 100 * p90, 100 * worst, early, late, hazard, hazard_worst, outside, push_us / pushes, static_cast<unsigned long long>(allocations));
    CHECK(flagged >= measured * 9 / 10 && median < 0.10 && p90 < 0.25 && early > late + 1.2 && hazard < 0.03 && outside == 0 && allocations == 0);
    if (grounded) std::printf("  a ground found on %u pictures of a still camera\n", grounded);
    CHECK(grounded == 0);
}
// The approach (TestLooming's scene), then a textured background panning under shake with the IMU: each picture's result and
// tile flows with every SIMD level this build and processor have (SSE2, AVX2) against the scalar loop, compared bit for bit;
// the time per picture of each.
void TestVectorised(core::LinearMemoryArena& arena) {
    const unsigned top = video::Perception::ForceLevel(2); // The best this build and processor have.
    if (!top) { std::printf("Vectorised: this build has no SIMD path (scalar only)\n"); return; }
    static Picture pics[2][70];
    for (unsigned f = 0; f < 70; ++f) {
        const double t = f / kFps, z = 3.0 - t * 30.0 / 70.0;
        pics[0][f].Draw(0, 0, true, 0.55 * kW, 0.45 * kH, 50 * 3.0 / z);
        pics[1][f].Draw(1, 90 * t + 4 * std::sin(2 * kPi * 7 * t), true, 0.4 * kW + 60 * t, 0.5 * kH, 40);
    }
    unsigned compared = 0, differing = 0; double us[2][3]{}; // [scene][level]
    for (unsigned scene = 0; scene < 2; ++scene) {
        video::Perception runs[3];
        for (unsigned level = 0; level <= top; ++level) CHECK(runs[level].Configure(arena, kW, kH, 0));
        for (unsigned f = 0; f < 70; ++f) {
            const video::EgoMotion ego{scene ? static_cast<float>((90 / kFps + 4 * (std::sin(2 * kPi * 7 * f / kFps) - std::sin(2 * kPi * 7 * (f - 1.0) / kFps))) / (kW / 2.0)) : 0.0f,
                                       0, 0, scene == 1 && f > 0};
            video::PerceptionResult out[3];
            for (unsigned level = 0; level <= top; ++level) {
                (void)video::Perception::ForceLevel(level);
                const auto began = std::chrono::steady_clock::now();
                out[level] = runs[level].Push(pics[scene][f].luma.data(), kW, Pts(f), ego);
                us[scene][level] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
            }
            for (unsigned level = 1; level <= top; ++level) {
                ++compared;
                const auto& a = out[level]; const auto& b = out[0];
                bool same = a.looming == b.looming && a.divergence == b.divergence && a.ttc_seconds == b.ttc_seconds && a.hazard_x == b.hazard_x &&
                            a.hazard_y == b.hazard_y && a.motion_x == b.motion_x && a.motion_y == b.motion_y && a.coverage == b.coverage && a.salient == b.salient &&
                            a.fovea == b.fovea && a.ground == b.ground && a.ground_windows == b.ground_windows;
                for (unsigned t = 0; t < video::Perception::kTiles * video::Perception::kTiles; ++t)
                    same = same && runs[level].FlowValid(t) == runs[0].FlowValid(t) && runs[level].FlowX(t) == runs[0].FlowX(t) &&
                           runs[level].FlowY(t) == runs[0].FlowY(t) && runs[level].Saliency(t) == runs[0].Saliency(t);
                differing += same ? 0u : 1u;
            }
        }
    }
    (void)video::Perception::ForceLevel(2);
    const char* names[]{"scalar", "SSE2", "AVX2"};
    std::printf("Vectorised: %u picture comparisons (an approach; a pan under shake with the IMU; every SIMD level against the scalar loop), %u differing; "
                "us per 640x480 picture, approach / pan:", compared, differing);
    for (unsigned level = 0; level <= top; ++level) std::printf(" %s %.0f / %.0f", names[level], us[0][level] / 70, us[1][level] / 70);
    std::printf("\n");
    CHECK(differing == 0 && compared == 140 * top);
    // The coarse level (2026-11-05, off by default): its costs at every SIMD level against the scalar loop, on the shaken pan.
    {
        video::Perception coarse[3]; unsigned coarse_differing = 0;
        for (unsigned level = 0; level <= top; ++level) { coarse[level].SetPyramid(true); CHECK(coarse[level].Configure(arena, kW, kH, 0)); }
        for (unsigned f = 0; f < 70; ++f)
            for (unsigned level = 0; level <= top; ++level) {
                (void)video::Perception::ForceLevel(level);
                (void)coarse[level].Push(pics[1][f].luma.data(), kW, Pts(f), {});
                if (level)
                    for (unsigned t = 0; t < video::Perception::kTiles * video::Perception::kTiles; ++t)
                        coarse_differing += coarse[level].FlowValid(t) != coarse[0].FlowValid(t) || coarse[level].FlowX(t) != coarse[0].FlowX(t) ||
                                            coarse[level].FlowY(t) != coarse[0].FlowY(t);
            }
        (void)video::Perception::ForceLevel(2);
        std::printf("Coarse level: %u tile differences between SIMD levels over 70 pictures\n", coarse_differing);
        CHECK(coarse_differing == 0 && coarse[0].Pyramid());
    }
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(us[0][top] / 70 < 600 && us[1][top] / 70 < 600); // The directives' budget, the approach and the fully textured pan (optimised builds only).
#endif
}
void TestTranslation(core::LinearMemoryArena& arena) {
    video::Perception p;
    CHECK(p.Configure(arena, kW, kH, 0));
    // The obstacle's texture moving right-down at (90, 45) px/s over a static textured background: no approach.
    Picture pic; unsigned looming = 0, measured = 0; double flow_error = 0; unsigned flow_tiles = 0; double background = 0;
    for (unsigned f = 0; f <= 45; ++f) {
        const double t = f / kFps;
        pic.Draw(1, 0, true, 200 + 90 * t, 160 + 45 * t, 70);
        const auto& r = p.Push(pic.luma.data(), kW, Pts(f), {});
        if (f < 4) continue;
        ++measured; if (r.looming) ++looming;
        background = (std::max)(background, std::hypot(static_cast<double>(r.motion_x), static_cast<double>(r.motion_y)));
        // Tiles well inside the object: its velocity.
        const double cx = 200 + 90 * t, cy = 160 + 45 * t;
        for (unsigned tile = 0; tile < 256; ++tile) {
            const double tx = (tile % 16 + 0.5) * kW / 16.0, ty = (tile / 16 + 0.5) * kH / 16.0;
            if (std::fabs(tx - cx) > 30 || std::fabs(ty - cy) > 30 || !p.FlowValid(tile)) continue;
            flow_error = (std::max)(flow_error, std::hypot(p.FlowX(tile) - 90.0, p.FlowY(tile) - 45.0)); ++flow_tiles;
        }
    }
    std::printf("Translation: looming on %u of %u pictures; %u object tiles, flow error worst %.1f px/s of (90, 45); dominant motion %.4f widths/s (the static background)\n",
                looming, measured, flow_tiles, flow_error, background);
    CHECK(looming == 0 && flow_tiles > 40 && flow_error < 12 && background < 0.01);
}
void TestEgo(core::LinearMemoryArena& arena) {
    // A pan to the right at 3 px a picture (90 px/s), the whole scene moving left: the IMU's yaw is 3 / f a picture.
    const double focal = kW / 2.0 / std::tan(kPi / 4);
    double with_imu = 0, without = 0; unsigned n = 0; bool ego_flag = true;
    video::Perception a, b;
    CHECK(a.Configure(arena, kW, kH, 0) && b.Configure(arena, kW, kH, static_cast<float>(kPi / 2)));
    Picture pic;
    for (unsigned f = 0; f <= 30; ++f) {
        pic.Draw(1, 3.0 * f, false, 0, 0, 0);
        const video::EgoMotion imu{f ? static_cast<float>(3.0 / focal) : 0.0f, 0, 0, true};
        const auto& ra = a.Push(pic.luma.data(), kW, Pts(f), imu);
        const auto& rb = b.Push(pic.luma.data(), kW, Pts(f), {});
        if (f < 4) continue;
        ++n; with_imu = (std::max)(with_imu, std::fabs(static_cast<double>(ra.motion_x)));
        without += rb.motion_x * static_cast<double>(kW); ego_flag = ego_flag && ra.ego && !rb.ego && !ra.looming && !rb.looming;
    }
    without /= n;
    std::printf("Ego-motion: a 90 px/s pan; with the IMU the dominant motion is %.4f widths/s at most (the scene's own: none), without it %.1f px/s\n", with_imu, without);
    CHECK(with_imu < 0.005 && std::fabs(without + 90) < 4 && ego_flag);
}
void TestSaliency(core::LinearMemoryArena& arena) {
    video::Perception p;
    CHECK(p.Configure(arena, kW, kH, 0));
    Picture pic; unsigned outside = 0, inside_miss = 0, frames = 0, still_salient = 0; float fraction = 0;
    for (unsigned f = 0; f <= 20; ++f) {
        // A small object moving left in the top-right corner; frames 15-20 the scene is still.
        const bool moving = f < 15;
        const double cx = 520 - (moving ? 4.0 * f : 56.0), cy = 90;
        pic.Draw(1, 0, true, cx, cy, 24);
        const auto& r = p.Push(pic.luma.data(), kW, Pts(f), {});
        if (f < 3) continue;
        if (!moving) { if (f >= 17) still_salient += r.salient; continue; }
        ++frames; fraction = (std::max)(fraction, r.salient_fraction);
        for (unsigned t = 0; t < 256; ++t) {
            const double x0 = (t % 16) * 40.0, y0 = (t / 16) * 30.0;
            const bool near = x0 + 40 > cx - 24 - 40 && x0 < cx + 24 + 40 && y0 + 30 > cy - 24 - 30 && y0 < cy + 24 + 30; // Within a tile of it.
            const bool centre = cx >= x0 && cx < x0 + 40 && cy >= y0 && cy < y0 + 30;
            if (p.Salient(t) && !near) ++outside;
            if (centre && !p.Salient(t)) ++inside_miss;
        }
    }
    std::printf("Saliency: %u pictures with a moving object: largest mask %.1f %% of the tiles, %u salient tiles away from it, its own tile missed %u times; a still scene: %u salient tiles\n",
                frames, 100 * fraction, outside, inside_miss, still_salient);
    CHECK(frames > 10 && fraction > 0 && fraction < 0.1f && outside == 0 && inside_miss == 0 && still_salient == 0);
}
// Camera shake (docs/picture.md, "Stabilisation"): a hand-held camera shaking in yaw, pitch and roll (+-1.5, 1.2 and 1 degrees
// at 7, 9 and 11 Hz) over a textured static scene, with a small object moving left in the top-right corner. Each picture is
// the scene as the rotated camera sees it (small angles: the content moves by (-f yaw + roll y, f pitch - roll x) about the
// centre). With the IMU: no looming, the dominant motion still (the scene's own: none), no salient tile away from the object;
// without it, the same pictures read as motion. The stabiliser moves the view with the shake: the scene point under the
// view's centre moves a fraction of what it does in the picture.
void TestShake(core::LinearMemoryArena& arena) {
    const double focal = kW / 2.0 / std::tan(kPi / 4);
    const auto angle = [](unsigned f, double degrees, double hz, double phase) { return degrees * kPi / 180 * std::sin(2 * kPi * hz * f / kFps + phase); };
    const auto rotation = [&](unsigned f, double& yaw, double& pitch, double& roll) {
        yaw = angle(f, 1.5, 7, 0); pitch = angle(f, 1.2, 9, 1); roll = angle(f, 1.0, 11, 2);
    };
    video::Perception with, without;
    CHECK(with.Configure(arena, kW, kH, 0) && without.Configure(arena, kW, kH, 0));
    video::Stabilizer stabilizer; stabilizer.Configure(kW, kH, 0);
    std::vector<std::uint8_t> luma(kW * kH);
    unsigned looming = 0, outside = 0, frames = 0, looming_without = 0, outside_without = 0, found = 0;
    double motion = 0, motion_without = 0, shaken = 0, residual = 0;
    double previous[3]{};
    for (unsigned f = 0; f <= 60; ++f) {
        double yaw, pitch, roll; rotation(f, yaw, pitch, roll);
        const double ox = 520 - 4.0 * f, oy = 90; // The object, in the world (which the camera's rotation moves).
        for (unsigned y = 0; y < kH; ++y)
            for (unsigned x = 0; x < kW; ++x) {
                double sum = 0;
                for (int s = 0; s < 4; ++s) {
                    const double px = x + 0.25 + 0.5 * (s & 1), py = y + 0.25 + 0.5 * (s >> 1), rx = px - kW / 2.0, ry = py - kH / 2.0;
                    const double wx = px - (-focal * yaw + roll * ry), wy = py - (focal * pitch - roll * rx); // The world point seen here.
                    double v = 40 + Noise(wx / 9.0, wy / 9.0, 3);
                    if (std::fabs(wx - ox) < 24 && std::fabs(wy - oy) < 24) v = 30 + Noise((wx - ox) / 4 + 50, (wy - oy) / 4 + 50, 11);
                    sum += v;
                }
                luma[y * kW + x] = static_cast<std::uint8_t>(std::clamp(std::lround(sum / 4), 0l, 255l));
            }
        const video::EgoMotion imu{static_cast<float>(yaw - previous[0]), static_cast<float>(pitch - previous[1]), static_cast<float>(roll - previous[2]), f > 0};
        previous[0] = yaw; previous[1] = pitch; previous[2] = roll;
        const auto& a = with.Push(luma.data(), kW, Pts(f), imu);
        const auto& b = without.Push(luma.data(), kW, Pts(f), {});
        stabilizer.Push(Pts(f), imu);
        if (f < 8) continue; // The low-pass settles; the flow has its baseline.
        ++frames;
        looming += a.looming; looming_without += b.looming;
        motion = (std::max)(motion, static_cast<double>(std::hypot(a.motion_x, a.motion_y)));
        motion_without = (std::max)(motion_without, static_cast<double>(std::hypot(b.motion_x, b.motion_y)));
        // The object, where the camera shows it now.
        const double sx = ox + (-focal * yaw + roll * (oy - kH / 2.0)), sy = oy + (focal * pitch - roll * (ox - kW / 2.0));
        for (unsigned t = 0; t < 256; ++t) {
            const double x0 = (t % 16) * 40.0, y0 = (t / 16) * 30.0;
            const bool near = x0 + 40 > sx - 24 - 40 && x0 < sx + 24 + 40 && y0 + 30 > sy - 24 - 30 && y0 < sy + 24 + 30;
            if (with.Salient(t) && !near) ++outside;
            if (without.Salient(t) && !near) ++outside_without;
        }
        bool seen = false; // The object itself: a tile of it in the mask.
        for (unsigned t = 0; t < 256; ++t)
            if (with.Salient(t) && std::fabs((t % 16) * 40.0 + 20 - sx) < 44 && std::fabs((t / 16) * 30.0 + 15 - sy) < 39) seen = true;
        found += seen;
        // The picture's centre moved by (-f yaw, f pitch) from where the camera means to look (the angles are all shake); the
        // view's centre moved by the stabiliser's shift: what is left is what the viewer sees shake.
        const double dx = -focal * yaw, dy = focal * pitch;
        shaken += dx * dx + dy * dy;
        const double rx = dx - stabilizer.ShiftX() * kW, ry = dy - stabilizer.ShiftY() * kH;
        residual += rx * rx + ry * ry;
    }
    shaken = std::sqrt(shaken / frames); residual = std::sqrt(residual / frames);
    float zoom = 1, view_x = 0.5f, view_y = 0.5f; stabilizer.Apply(zoom, view_x, view_y);
    std::printf("Shake (+-1.5/1.2/1.0 degrees yaw/pitch/roll at 7/9/11 Hz, %u pictures): with the IMU looming %u, dominant motion %.4f widths/s at most, "
                "%u salient tile-pictures away from the object, the object salient on %u; without it looming %u, motion %.3f, %u away; the picture's centre "
                "shakes %.1f px RMS, the stabilised view %.1f px (zoom %.2f)\n", frames, looming, motion, outside, found, looming_without, motion_without,
                outside_without, shaken, residual, static_cast<double>(zoom));
    CHECK(frames >= 50 && looming == 0 && motion < 0.01 && outside * 10 <= frames && found * 10 >= frames * 9);
    CHECK(motion_without > 0.1 && outside_without > outside); // The pictures do shake: the IMU is what tells it apart.
    CHECK(shaken > 5 && residual < 0.25 * shaken && zoom >= video::Stabilizer::kZoom);
}
// Audio: 48 kHz stereo; a quiet noise bed, then a burst of noise (20 ms, -12 dBFS) placed by gains and a delay.
struct Bed {
    std::vector<float> frames;
    void Make(double seconds, std::uint32_t seed) {
        frames.assign(static_cast<std::size_t>(seconds * 48000) * 2, 0);
        for (std::size_t i = 0; i < frames.size(); ++i) frames[i] = 0.001f * (static_cast<float>(Hash(static_cast<std::uint32_t>(i) + seed) % 2001u) / 1000.0f - 1.0f);
    }
    void Burst(double at, double gain_left, double gain_right, int right_delay) {
        const auto start = static_cast<std::size_t>(at * 48000);
        for (std::size_t k = 0; k < 960; ++k) {
            const float s = 0.35f * (static_cast<float>(Hash(static_cast<std::uint32_t>(k) * 31u + 99u) % 2001u) / 1000.0f - 1.0f);
            frames[2 * (start + k)] += static_cast<float>(gain_left) * s;
            const auto r = start + k + static_cast<std::size_t>(right_delay);
            if (2 * r + 1 < frames.size()) frames[2 * r + 1] += static_cast<float>(gain_right) * s;
        }
    }
};
void TestLocator() {
    struct Case { const char* name; double left, right; int delay; double expect; };
    const double pan = 0.5, gl = std::cos((pan + 1) * kPi / 4), gr = std::sin((pan + 1) * kPi / 4);
    // 0.3 ms: the right channel hears it 14.4 samples later, so it came from the left: -0.3e-3 x 343 / 0.2.
    const Case cases[]{{"right only", 0, 1, 0, 1}, {"left only", 1, 0, 0, -1}, {"panned +0.5", gl, gr, 0, 0.5}, {"left leads 0.3 ms (0.2 m pair)", 1, 1, 14, -0.3e-3 * 343 / 0.2 * 14 / 14.4}};
    unsigned good = 0; std::uint64_t allocations = 0;
    for (const auto& c : cases) {
        audio::SoundLocator l; CHECK(l.Configure(48000, 2, 0.2f));
        Bed bed; bed.Make(1.0, 5); bed.Burst(0.5, c.left, c.right, c.delay);
        allocation_probe::Begin();
        for (std::size_t at = 0; at < bed.frames.size() / 2; at += 480) l.Process(bed.frames.data() + 2 * at, 480, static_cast<std::int64_t>(at) * 10000000 / 48000);
        allocations += allocation_probe::End();
        audio::SoundEvent e; const bool found = l.Latest(e);
        const bool ok = found && e.sequence == 1 && std::fabs(e.azimuth - c.expect) < 0.05 && e.pts_100ns >= 4900000 && e.pts_100ns <= 5100000;
        std::printf("Locator %-32s azimuth %+.3f (expected %+.3f), ILD %+.1f dB, ITD %+.0f us, correlation %.2f, level %.1f dB, at %.3f s, events %u%s\n", c.name, e.azimuth, c.expect,
                    e.ild_db, e.itd_us, e.correlation, e.level_db, e.pts_100ns / 1e7, l.Events(), ok ? "" : "  WRONG");
        good += ok;
    }
    // The bed alone: nothing. Two bursts 100 ms apart: one event (refractory), 400 ms apart: two.
    audio::SoundLocator quiet; CHECK(quiet.Configure(48000, 2, 0));
    Bed bed; bed.Make(1.0, 6);
    quiet.Process(bed.frames.data(), static_cast<std::uint32_t>(bed.frames.size() / 2), 0);
    audio::SoundLocator twice; CHECK(twice.Configure(48000, 2, 0));
    Bed two; two.Make(1.5, 7); two.Burst(0.3, 0, 1, 0); two.Burst(0.4, 1, 0, 0); two.Burst(0.8, 1, 0, 0);
    twice.Process(two.frames.data(), static_cast<std::uint32_t>(two.frames.size() / 2), 0);
    audio::SoundEvent last{}; (void)twice.Latest(last);
    std::printf("Locator: %u of 4 placed; the noise bed alone %u events; bursts at 0.3, 0.4, 0.8 s: %u events (the last %+.2f); %llu allocations\n", good, quiet.Events(),
                twice.Events(), last.azimuth, static_cast<unsigned long long>(allocations));
    audio::SoundLocator mono;
    CHECK(good == 4 && quiet.Events() == 0 && twice.Events() == 2 && last.azimuth < -0.95f && allocations == 0 && !mono.Configure(48000, 1, 0));
}
void TestCommands() {
    ai::MediaBounds film; film.duration_100ns = 600000000; film.position_100ns = 100000000; film.seekable = true;
    const char* on[]{"look at sound", "look at the sound", "follow the sound", "Look towards the sound please", "audio saccade",
                     "\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x94\xd7\xa7\xd7\x95\xd7\x9c",                 // עקוב אחרי הקול
                     "\xd7\xaa\xd7\xa1\xd7\xaa\xd7\x9b\xd7\x9c \xd7\x9c\xd7\x9b\xd7\x99\xd7\x95\xd7\x95\xd7\x9f \xd7\x94\xd7\xa7\xd7\x95\xd7\x9c", // תסתכל לכיוון הקול
                     "\xd8\xa7\xd8\xaa\xd8\xa8\xd8\xb9 \xd8\xa7\xd9\x84\xd8\xb5\xd9\x88\xd8\xaa", "sigue el sonido", "regarde le son", "folge dem Ger\xc3\xa4usch",
                     "\xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd0\xb8 \xd0\xb7\xd0\xb0 \xd0\xb7\xd0\xb2\xd1\x83\xd0\xba\xd0\xbe\xd0\xbc", "\xe8\xb7\x9f\xe9\x9a\x8f\xe5\xa3\xb0\xe9\x9f\xb3",
                     "\xe9\x9f\xb3\xe3\x82\x92\xe8\xbf\xbd\xe3\x81\xa3\xe3\x81\xa6"};
    const char* off[]{"stop following the sound", "audio saccade off", "ignore sounds",
                      "\xd7\xaa\xd7\xa4\xd7\xa1\xd7\x99\xd7\xa7 \xd7\x9c\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x94\xd7\xa7\xd7\x95\xd7\x9c", // תפסיק לעקוב אחרי הקול
                      "arrete de suivre le son", "\xe5\x81\x9c\xe6\xad\xa2\xe8\xb7\x9f\xe9\x9a\x8f\xe5\xa3\xb0\xe9\x9f\xb3"};
    unsigned right = 0, total = 0;
    const auto expect = [&](const char* t, ai::IntentKind kind, bool any_status = false) {
        ++total; const auto i = ai::LocalIntentDispatcher::Parse(t, film);
        if ((i.status == ai::IntentStatus::Ok || any_status) && i.kind == kind) ++right; else std::printf("  \"%s\" -> %s %s\n", t, ai::Name(i.kind), ai::Name(i.status));
    };
    for (const char* t : on) expect(t, ai::IntentKind::AudioSaccadeOn);
    for (const char* t : off) expect(t, ai::IntentKind::AudioSaccadeOff);
    // Neighbours keep their meaning.
    expect("follow the object", ai::IntentKind::TrackTarget);
    expect("\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x94\xd7\x90\xd7\x95\xd7\x91\xd7\x99\xd7\x99\xd7\xa7\xd7\x98", ai::IntentKind::TrackTarget); // עקוב אחרי האובייקט
    expect("stop tracking", ai::IntentKind::StopTracking);
    expect("volume up", ai::IntentKind::VolumeUp);
    expect("\xd7\x94\xd7\x92\xd7\x91\xd7\xa8 \xd7\xa7\xd7\x95\xd7\x9c", ai::IntentKind::VolumeUp, true); // הגבר קול (still volume; without an amount it asks for one)
    expect("stop", ai::IntentKind::Pause);
    std::printf("Commands: %u of %u as expected (the saccade in nine languages, its neighbours unchanged)\n", right, total);
    CHECK(right == total && ai::LocalIntentDispatcher::TablesConsistent());
}
}

int main() {
    core::LinearMemoryArena arena;
    if (!arena.Initialize(8u << 20)) { std::printf("FAIL: arena\n"); return 1; }
    TestLooming(arena);
    TestVectorised(arena);
    TestTranslation(arena);
    TestEgo(arena);
    TestSaliency(arena);
    TestShake(arena);
    TestLocator();
    TestCommands();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "perception tests passed", failures);
    return failures ? 1 : 0;
}
