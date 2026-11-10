// Target tracking (docs/picture.md, "Target tracking"; AI tasks A11, A35): video::TargetTracker on synthetic sequences whose
// truth is known, the view and the audio focus it steers, and the commands that start and stop it. Portable; MSVC and GCC.
// - the grid: MakeGrid against a direct computation, GridScale for the usual sizes;
// - a textured target moving on a textured background (linear, then a circle, sub-sample speeds, noise of +-2 codes): the
//   estimate's error, the view that follows it at 2x, and a sound panned with the target that the audio focus brings forward;
// - a whole-picture brightness change: still tracked (ZNCC); a flat patch: refused;
// - occlusion: lost within two pictures, the view held still while lost, found again after it;
// - a scene cut: lost at once and never "found" in the new shot;
// - "track target", "follow the object", "עקוב אחרי האובייקט", "stop tracking", "הפסק מעקב" and the other languages;
// - no allocation while tracking.
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/audio/MasteringEq.hpp"
#include "media/video/TargetTracker.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr double kPi = 3.14159265358979323846;
std::uint32_t Hash(std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
// A picture: a smooth-ish textured background (8-sample blobs), a textured square target centred at (tx, ty) (samples,
// fractional: drawn with area coverage), an optional occluder, noise.
struct Scene {
    unsigned width = 960, height = 540, size = 24; std::uint32_t seed = 7;
    float brightness = 1.0f; bool occlude = false; float occluder_x = 0, occluder_y = 0; bool other_shot = false;
};
void Draw(const Scene& s, float tx, float ty, unsigned frame, std::vector<std::uint8_t>& luma) {
    luma.assign(static_cast<std::size_t>(s.width) * s.height, 0);
    const float half = s.size / 2.0f;
    for (unsigned y = 0; y < s.height; ++y)
        for (unsigned x = 0; x < s.width; ++x) {
            const std::uint32_t seed = s.other_shot ? s.seed * 977u + 13u : s.seed;
            float v = 60.0f + static_cast<float>(Hash((x / 8) * 7919u + (y / 8) * 104729u + seed) % 90u);
            // The target: a texture of its own, not periodic (3-sample cells of two shades, and a diagonal ramp), smaller than the
            // template, so its outline is inside it.
            const float dx = static_cast<float>(x) + 0.5f - tx, dy = static_cast<float>(y) + 0.5f - ty;
            const float cover = std::clamp(half + 0.5f - std::fabs(dx), 0.0f, 1.0f) * std::clamp(half + 0.5f - std::fabs(dy), 0.0f, 1.0f);
            if (cover > 0 && !s.other_shot) {
                const int lx = static_cast<int>(std::floor(dx + half)), ly = static_cast<int>(std::floor(dy + half));
                const float t = (Hash(static_cast<std::uint32_t>(lx / 3) * 131u + static_cast<std::uint32_t>(ly / 3) * 7u + 5u) & 1u) ? 230.0f : 25.0f;
                const float target = std::clamp(t + 0.6f * static_cast<float>(lx - ly), 0.0f, 255.0f);
                v = v * (1 - cover) + target * cover;
            }
            if (s.occlude && std::fabs(static_cast<float>(x) - s.occluder_x) < 45 && std::fabs(static_cast<float>(y) - s.occluder_y) < 45) v = 128;
            v = v * s.brightness + static_cast<float>(static_cast<int>(Hash(x * 31u + y * 1009u + frame * 7777u) % 5u) - 2);
            luma[static_cast<std::size_t>(y) * s.width + x] = static_cast<std::uint8_t>(std::clamp(std::lround(v), 0l, 255l));
        }
}
struct Grid {
    std::vector<std::uint8_t> cells; unsigned width = 0, height = 0, scale = 1;
    video::LumaGrid View() const { return {cells.data(), width, height, width}; }
    void Make(const std::vector<std::uint8_t>& luma, unsigned w, unsigned h) {
        scale = video::GridScale(w, h); width = (w + scale - 1) / scale; height = (h + scale - 1) / scale;
        cells.resize(static_cast<std::size_t>(width) * height);
        video::MakeGrid(luma.data(), w, w, h, scale, cells.data(), width);
    }
};
void GridChecks() {
    CHECK(video::GridScale(64, 64) == 1 && video::GridScale(1280, 720) == 3 && video::GridScale(1920, 1080) == 4 && video::GridScale(3840, 2160) == 8 &&
          video::GridScale(960, 540) == 2 && video::GridScale(481, 10) == 2);
    std::vector<std::uint8_t> luma(37 * 23); for (std::size_t i = 0; i < luma.size(); ++i) luma[i] = static_cast<std::uint8_t>(Hash(static_cast<std::uint32_t>(i)) & 255);
    std::uint8_t out[13 * 8]{}; video::MakeGrid(luma.data(), 37, 37, 23, 3, out, 13);
    bool exact = true;
    for (unsigned gy = 0; gy < 8; ++gy) for (unsigned gx = 0; gx < 13; ++gx) {
        unsigned sum = 0, n = 0;
        for (unsigned y = gy * 3; y < (std::min)(23u, gy * 3 + 3); ++y) for (unsigned x = gx * 3; x < (std::min)(37u, gx * 3 + 3); ++x) { sum += luma[y * 37 + x]; ++n; }
        exact = exact && out[gy * 13 + gx] == (sum + n / 2) / n;
    }
    CHECK(exact);
}
// Goertzel power of `hz` in a mono signal.
double Power(const std::vector<float>& x, double hz, double rate) {
    const double w = 2 * kPi * hz / rate, c = 2 * std::cos(w); double s1 = 0, s2 = 0;
    for (float v : x) { const double s = v + c * s1 - s2; s2 = s1; s1 = s; }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (static_cast<double>(x.size()) * x.size());
}
void Moving() {
    Scene scene;
    Grid grid; std::vector<std::uint8_t> luma;
    // Path: 80 pictures across the picture at 9 samples a picture (4.5 cells), then 80 round a circle of 90 samples.
    const auto path = [](unsigned f, float& x, float& y) {
        if (f < 80) { x = 120.0f + 9.0f * static_cast<float>(f); y = 270.0f + 0.375f * static_cast<float>(f); return; }
        const double a = 2 * kPi * (f - 80) / 80.0; x = static_cast<float>(750 + 90 * std::cos(a)); y = static_cast<float>(300 + 90 * std::sin(a));
    };
    float x0, y0; path(0, x0, y0); Draw(scene, x0, y0, 0, luma); grid.Make(luma, scene.width, scene.height);
    video::TargetTracker tracker;
    // The view's centre was put on the target (as the user does before "track target").
    CHECK(tracker.Lock(grid.View(), x0 / scene.width, y0 / scene.height));
    double sum2 = 0, worst = 0, match2 = 0, match_worst = 0; unsigned tracked = 0;
    // The audio: tone A panned left (0.2) and tone B right (0.8), 1 kHz and 6.25 kHz (as far from the focus's 2.5 kHz presence
    // band either way); the focus follows the view as the engine does (zoom 2.4: full).
    audio::AudioFocus focus; CHECK(focus.Configure(48000, 2));
    double a_start = 0, b_start = 0, a_end = 0, b_end = 0; unsigned phase = 0;
    std::vector<float> block(2 * 2000), mono_a, mono_b;
    std::uint64_t heap = 0;
    for (unsigned f = 1; f < 160; ++f) {
        float tx, ty; path(f, tx, ty); Draw(scene, tx, ty, f, luma); grid.Make(luma, scene.width, scene.height);
        allocation_probe::Begin();
        const auto st = tracker.Update(grid.View(), false);
        heap += allocation_probe::End();
        if (st.state != video::TrackState::Tracking) {
            std::printf("  picture %u: lost (confidence %.2f) at (%.1f, %.1f)\n", f, static_cast<double>(st.confidence), static_cast<double>(tx), static_cast<double>(ty));
            continue;
        }
        ++tracked;
        const double ex = (st.x * scene.width - tx) / grid.scale, ey = (st.y * scene.height - ty) / grid.scale; // In cells.
        const double e = std::sqrt(ex * ex + ey * ey); sum2 += e * e; worst = (std::max)(worst, e);
        const double mx = (st.match_x * scene.width - tx) / grid.scale, my = (st.match_y * scene.height - ty) / grid.scale;
        const double m = std::sqrt(mx * mx + my * my); match2 += m * m; match_worst = (std::max)(match_worst, m);
        // The view at 2x follows (kept inside the picture); the audio focus takes its x.
        const float zoom = 2.4f, half = 0.5f / zoom, view_x = std::clamp(st.x, half, 1 - half);
        focus.Set(true, zoom, view_x);
        for (unsigned i = 0; i < 2000; ++i, ++phase) {
            const double a = 0.3 * std::sin(2 * kPi * 1000 * phase / 48000.0), b = 0.3 * std::sin(2 * kPi * 6250 * phase / 48000.0);
            const double pa = 0.2 * kPi / 2, pb = 0.8 * kPi / 2;
            block[2 * i] = static_cast<float>(a * std::cos(pa) + b * std::cos(pb)); block[2 * i + 1] = static_cast<float>(a * std::sin(pa) + b * std::sin(pb));
        }
        focus.Process(block.data(), 2000);
        if (f == 12 || f == 76) {
            std::vector<float> left(2000), right(2000); for (unsigned i = 0; i < 2000; ++i) { left[i] = block[2 * i]; right[i] = block[2 * i + 1]; }
            const double pa = Power(left, 1000, 48000) + Power(right, 1000, 48000), pb = Power(left, 6250, 48000) + Power(right, 6250, 48000);
            if (f == 12) { a_start = pa; b_start = pb; } else { a_end = pa; b_end = pb; }
        }
    }
    const double rms = std::sqrt(sum2 / (tracked ? tracked : 1)), match = std::sqrt(match2 / (tracked ? tracked : 1));
    std::printf("Moving target (960x540, grid %ux%u of %u-sample cells, 4.5 cells a picture, then a circle): tracked %u of 159 pictures; match error RMS %.3f cells "
                "(%.2f samples), worst %.3f; smoothed (the view) RMS %.3f, worst %.3f cells\n",
                grid.width, grid.height, grid.scale, tracked, match, match * grid.scale, match_worst, rms, worst);
    // The match is sub-cell; the smoothed estimate lags a target that starts at full speed or turns sharply (alpha-beta filter).
    CHECK(tracked == 159 && match < 0.3 && match_worst < 0.8 && rms < 0.5 && worst < 2.5);
    const auto db = [](double p, double q) { return 10 * std::log10(p / q); };
    std::printf("  audio focus steered by the view: left tone %.1f dB over the right while the target is left, %.1f dB under it once it is right\n",
                db(a_start, b_start), -db(a_end, b_end));
    CHECK(db(a_start, b_start) > 2 && db(b_end, a_end) > 2);
    CHECK(heap == 0);
}
void Robustness() {
    Scene scene; Grid grid; std::vector<std::uint8_t> luma;
    video::TargetTracker tracker;
    // A flat patch: nothing to follow.
    std::vector<std::uint8_t> flat(960 * 540, 100); grid.Make(flat, 960, 540);
    CHECK(!tracker.Lock(grid.View(), 0.5f, 0.5f) && tracker.Status().state == video::TrackState::Off);
    // Brightness: the whole picture 25 % brighter over 10 pictures, the target still and then moving.
    Draw(scene, 480, 270, 0, luma); grid.Make(luma, 960, 540); CHECK(tracker.Lock(grid.View(), 0.5f, 0.5f));
    unsigned kept = 0;
    for (unsigned f = 1; f <= 20; ++f) {
        scene.brightness = 1.0f + 0.025f * static_cast<float>((std::min)(f, 10u));
        const float tx = 480 + 2.0f * static_cast<float>(f);
        Draw(scene, tx, 270, f, luma); grid.Make(luma, 960, 540);
        const auto st = tracker.Update(grid.View(), false);
        if (st.state == video::TrackState::Tracking && std::fabs(st.x * 960 - tx) < 3) ++kept;
    }
    std::printf("Brightness ramp +25 %%: on target %u of 20 pictures\n", kept);
    CHECK(kept == 20);
    // Occlusion: a grey card over the target for 12 pictures while it keeps moving 2 samples a picture.
    scene.brightness = 1;
    float tx = 520; Draw(scene, tx, 270, 100, luma); grid.Make(luma, 960, 540); CHECK(tracker.Lock(grid.View(), tx / 960, 0.5f));
    for (unsigned f = 0; f < 5; ++f) { tx += 2; Draw(scene, tx, 270, 101 + f, luma); grid.Make(luma, 960, 540); (void)tracker.Update(grid.View(), false); }
    unsigned lost_after = 99, found_after = 99; float held_x = -1, worst_jump = 0;
    for (unsigned f = 0; f < 30; ++f) {
        tx += 2;
        scene.occlude = f < 12; scene.occluder_x = tx; scene.occluder_y = 270;
        Draw(scene, tx, 270, 200 + f, luma); grid.Make(luma, 960, 540);
        const auto st = tracker.Update(grid.View(), false);
        if (st.state == video::TrackState::Lost) {
            if (lost_after == 99) lost_after = f;
            if (held_x < 0) held_x = st.x; else worst_jump = (std::max)(worst_jump, std::fabs(st.x - held_x) * 960);
        } else if (lost_after != 99 && found_after == 99 && f >= 12) found_after = f - 12;
        if (f == 29) CHECK(st.state == video::TrackState::Tracking && std::fabs(st.x * 960 - tx) < 3);
    }
    std::printf("Occlusion (12 pictures): lost after %u picture(s), view held (moved %.2f samples while lost), found %u picture(s) after it ended\n",
                lost_after + 1, worst_jump, found_after);
    CHECK(lost_after <= 1 && worst_jump == 0 && found_after <= 3);
    // A scene cut: lost at once, held, never found in the other shot.
    Draw(scene, 480, 270, 300, luma); scene.occlude = false; Draw(scene, 480, 270, 300, luma); grid.Make(luma, 960, 540); CHECK(tracker.Lock(grid.View(), 0.5f, 0.5f));
    scene.other_shot = true; unsigned found = 0; bool held = true;
    for (unsigned f = 0; f < 30; ++f) {
        Draw(scene, 480, 270, 301 + f, luma); grid.Make(luma, 960, 540);
        const auto st = tracker.Update(grid.View(), f == 0);
        if (f == 0) CHECK(st.state == video::TrackState::Lost);
        if (st.state == video::TrackState::Tracking) ++found;
        held = held && std::fabs(st.x - 0.5f) < 0.02f;
    }
    std::printf("Scene cut: lost on the cut picture; found in the new shot %u of 30 times; view held: %s\n", found, held ? "yes" : "NO");
    CHECK(found == 0 && held);
}
void Commands() {
    const ai::MediaBounds film{6000000000ll, 1200000000ll, 0.5f, false, false, true, true, 0, 0, 1, 1};
    const char* on[]{"track target", "follow the object", "lock on target", "Track the target please",
                     "\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x94\xd7\x90\xd7\x95\xd7\x91\xd7\x99\xd7\x99\xd7\xa7\xd7\x98", // עקוב אחרי האובייקט
                     "\xd7\xaa\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91 \xd7\x90\xd7\x97\xd7\xa8\xd7\x99 \xd7\x94\xd7\x9e\xd7\x98\xd7\xa8\xd7\x94",                // תעקוב אחרי המטרה
                     "\xd8\xaa\xd8\xaa\xd8\xa8\xd8\xb9 \xd8\xa7\xd9\x84\xd9\x87\xd8\xaf\xd9\x81", "seguir el objetivo", "suivre la cible", "Ziel verfolgen",
                     "\xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd0\xb8 \xd0\xb7\xd0\xb0 \xd1\x86\xd0\xb5\xd0\xbb\xd1\x8c\xd1\x8e", "\xe8\xb7\x9f\xe8\xb8\xaa\xe7\x9b\xae\xe6\xa0\x87",
                     "\xe3\x82\xbf\xe3\x83\xbc\xe3\x82\xb2\xe3\x83\x83\xe3\x83\x88\xe3\x82\x92\xe8\xbf\xbd\xe8\xb7\xa1"};
    const char* off[]{"stop tracking", "tracking off", "stop following the object",
                      "\xd7\x94\xd7\xa4\xd7\xa1\xd7\xa7 \xd7\x9e\xd7\xa2\xd7\xa7\xd7\x91", "\xd7\xaa\xd7\xa4\xd7\xa1\xd7\x99\xd7\xa7 \xd7\x9c\xd7\xa2\xd7\xa7\xd7\x95\xd7\x91", // הפסק מעקב, תפסיק לעקוב
                      "arreter le suivi", "Verfolgung beenden", "\xe5\x81\x9c\xe6\xad\xa2\xe8\xb7\x9f\xe8\xb8\xaa"};
    unsigned right = 0, total = 0;
    for (const char* t : on) { ++total; const auto i = ai::LocalIntentDispatcher::Parse(t, film); if (i.status == ai::IntentStatus::Ok && i.kind == ai::IntentKind::TrackTarget) ++right; else std::printf("  \"%s\" -> %s %s\n", t, ai::Name(i.kind), ai::Name(i.status)); }
    for (const char* t : off) { ++total; const auto i = ai::LocalIntentDispatcher::Parse(t, film); if (i.status == ai::IntentStatus::Ok && i.kind == ai::IntentKind::StopTracking) ++right; else std::printf("  \"%s\" -> %s %s\n", t, ai::Name(i.kind), ai::Name(i.status)); }
    // Neighbours keep their meaning: track selection is not tracking.
    const auto next_track = ai::LocalIntentDispatcher::Parse("next audio track", film);
    std::printf("Commands: %u of %u read as tracking; \"next audio track\" -> %s\n", right, total, ai::Name(next_track.kind));
    CHECK(right == total && next_track.kind == ai::IntentKind::SelectAudio);
    CHECK(ai::LocalIntentDispatcher::TablesConsistent());
}
}

int main() {
    GridChecks();
    Moving();
    Robustness();
    Commands();
    std::printf("Target tracking: %d failures\n", failures);
    return failures ? 1 : 0;
}
