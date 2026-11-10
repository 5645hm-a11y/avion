// Auto-leveling the horizon (docs/picture.md, "Auto-leveling the horizon", 2026-10-29): video::HorizonLevel and, on Windows,
// the statistics pass's edge-orientation histograms (HdrToneMap.hlsl Orientation) through the production colour pipeline.
// The scene (horizon_scene.hpp): sky over a horizon at 0.45 of the height, three buildings standing on it (dark blocks with lit
// windows: horizontal and vertical edges), a textured ground (edges every way), drawn turned clockwise by the tilt about the
// picture's centre as a camera sees it (4 x 4 samples a pixel, a 0.7 px blur: drawn with 2 x 2 samples and no blur, the edges'
// staircase read 0.5 to 0.6 degree off; a camera's edges are not staircases).
// Portable:
// - the CPU reference of the histograms (HorizonLevel::Accumulate) and Measure at 1280x720: +4.5, -6.0, 0, +12 and -14 degrees,
//   each within 0.35 degree;
// - a sequence of 90 pictures at 30 a second (+4.5 degrees, then -6.0 from picture 30), each with its own noise and every
//   seventh with a bright pole lying at +11 degrees across a third of the picture: locked, the level within 0.35 degree of
//   each tilt before the switch and at the end, never past either by more than that (a glide, not a swing);
// - no allocation in Accumulate, Measure or Push.
// Black bars and perspective (docs/picture.md, "Black bars", 2026-10-30), portable:
// - the scene tilted +4.5 and -6.0 degrees inside a 2.39:1 letterbox (92 rows of black, the encoder's +-1 code of noise) and a
//   4:3 pillarbox (160 columns): the tilt within 0.35 degree, where the same histograms without the bar test (black taken as
//   code 0, so the bars are not black) read the bars' edges;
// - a facade in two-point perspective (its horizontal edges receding at +7 degrees, its verticals vertical): no tilt
//   (`perspective`); the same facade level, and rolled +4.5 degrees as a whole: 0 and +4.5;
// - video::Matte on 45 pictures at 30 a second (each with its own noise): the letterbox's and the pillarbox's active area within
//   2 samples, locked after 1 s; no bars in the scene alone; a burned-in subtitle in the bottom bar makes it thinner at once; a
//   dark sky whose brightest point moves from picture to picture is no bar; no allocation; the CPU survey's time (the
//   histograms and the extents, what the SDK's host mode does per picture).
// Windows, with a hardware D3D12 device: each tilted picture (NV12, 1280x720) converted with kHorizon; the histograms read
// back (1024 bytes, the only thing that leaves the GPU) measure the tilt within 0.35 degree, and agree with the CPU
// reference (each class's total weight within 3 %); converted again with the leveling turn (kRotate, video::Orientation's
// map, level = minus the tilt measured) the output's own lines measure level within 0.35 degree; the statistics pass's GPU
// time at 3840x2160 with and without the histograms.
// Windows, with the GPU, black bars: the letterboxed and pillarboxed tilted pictures measure their tilt within 0.35 degree from the
// GPU's histograms, and the statistics pass's extents (kMatte) are the CPU reference's words exactly; the pass's time with the
// extents alone.
// Windows, real footage (the fixtures directory as the argument; skipped without it): costa_rica_4k60_hdr.mp4 (720p59.94
// nature footage: a beach, the sea's horizon at the left, a tree line) decoded by Media Foundation for this test only, 60
// pictures, a 960x540 window of each turned +4.5 and -6.0 degrees about its centre (bilinear) as a camera's roll would, with and
// without a 2.39:1 letterbox: the median tilt found and the estimator's locked level against the footage's own tilt (the same
// window unturned) plus the roll, within 0.35 degree; one turned, letterboxed picture through the GPU against the CPU's
// measurement.
#include "media/video/HorizonLevel.hpp"
#include "media/video/Orientation.hpp"
#include "allocation_probe.hpp"
#include "horizon_scene.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "media/video/Matte.hpp"
#if defined(_WIN32)
#include "media/render/ColorPipeline.hpp"
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

using media::video::HorizonLevel;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr double kPi = 3.14159265358979323846;
using horizon_scene::Draw;
using horizon_scene::Hash;
HorizonLevel::Measurement MeasureCpu(const std::vector<std::uint8_t>& luma, unsigned w, unsigned h) {
    std::uint32_t words[HorizonLevel::kWords]{};
    HorizonLevel::Accumulate(luma.data(), w, w, h, words);
    return HorizonLevel::Measure(words);
}

void TestStill() {
    const double tilts[]{4.5, -6.0, 0.0, 12.0, -14.0};
    double worst = 0; unsigned found = 0;
    std::vector<std::uint8_t> luma;
    std::printf("Still pictures (1280x720, CPU histograms):");
    for (const double t : tilts) {
        Draw(t, 1280, 720, luma);
        const auto m = MeasureCpu(luma, 1280, 720);
        std::printf(" %+.1f -> %+.2f (mass %.0f)", t, static_cast<double>(m.tilt), static_cast<double>(m.mass));
        found += m.found; worst = (std::max)(worst, std::fabs(m.tilt - t));
    }
    std::printf("; worst %.2f degree\n", worst);
    CHECK(found == 5 && worst <= 0.35);
}
void TestSequence() {
    constexpr unsigned W = 640, H = 360, kPictures = 90;
    std::vector<std::uint8_t> base[2], luma(W * H);
    Draw(4.5, W, H, base[0]); Draw(-6.0, W, H, base[1]);
    HorizonLevel level;
    std::uint64_t allocations = 0; double before_switch = 99, at_end = 99, overshoot = 0; unsigned locked_at = 0, found = 0;
    for (unsigned f = 0; f < kPictures; ++f) {
        const double truth = f < 30 ? 4.5 : -6.0;
        const auto& b = base[f < 30 ? 0 : 1];
        // The picture's own noise (+-3 codes), and every seventh picture a bright pole at +11 degrees through a third of it.
        for (unsigned i = 0; i < W * H; ++i) luma[i] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(b[i]) + static_cast<int>(Hash(i * 31u + f * 977u) % 7u) - 3, 0, 255));
        if (f % 7 == 3) {
            const double r = 11.0 * kPi / 180, x0 = 0.1 * W + 3.0 * f, y0 = 0.6 * H;
            for (double s = 0; s < W / 3.0; s += 0.25)
                for (int k = -2; k <= 2; ++k) {
                    const int x = static_cast<int>(x0 + s * std::cos(r) - k * std::sin(r)), y = static_cast<int>(y0 + s * std::sin(r) + k * std::cos(r));
                    if (x >= 0 && y >= 0 && x < static_cast<int>(W) && y < static_cast<int>(H)) luma[static_cast<std::size_t>(y) * W + x] = 235;
                }
        }
        std::uint32_t words[HorizonLevel::kWords]{};
        allocation_probe::Begin();
        HorizonLevel::Accumulate(luma.data(), W, W, H, words);
        const auto m = HorizonLevel::Measure(words);
        level.Push(static_cast<std::int64_t>(f) * 333333, m);
        allocations += allocation_probe::End();
        found += m.found;
        if (level.Locked() && !locked_at) locked_at = f + 1;
        if (f == 29) before_switch = std::fabs(level.Tilt() - truth);
        if (f == kPictures - 1) at_end = std::fabs(level.Tilt() - truth);
        overshoot = (std::max)({overshoot, static_cast<double>(level.Tilt()) - 4.5, -6.0 - static_cast<double>(level.Tilt())});
    }
    std::printf("Sequence (90 pictures at 30/s, +4.5 then -6.0 degrees, a pole at +11 degrees every seventh): measured on %u, locked after %u pictures; "
                "the level %.2f degree from +4.5 at picture 30, %.2f from -6.0 at the end, past either by %.2f at most; %llu allocations\n",
                found, locked_at, before_switch, at_end, (std::max)(overshoot, 0.0), static_cast<unsigned long long>(allocations));
    CHECK(found >= 80 && locked_at && locked_at <= 8 && before_switch <= 0.35 && at_end <= 0.35 && overshoot <= 0.35 && allocations == 0);
}

// A picture drawn as a camera sees it (horizon_scene::Draw's 4 x 4 samples a pixel and [1 2 1] / 4 blur) from `scene` (u, v)
// turned clockwise by `roll` degrees about the centre.
template <class Scene> void Render(double roll, unsigned w, unsigned h, std::vector<std::uint8_t>& luma, Scene scene) {
    std::vector<double> area(static_cast<std::size_t>(w) * h);
    const double r = roll * kPi / 180, co = std::cos(r), si = std::sin(r), cx = w / 2.0, cy = h / 2.0;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            double sum = 0;
            for (int k = 0; k < 16; ++k) {
                const double px = x + 0.125 + 0.25 * (k & 3) - cx, py = y + 0.125 + 0.25 * (k >> 2) - cy;
                sum += scene(co * px + si * py + cx, -si * px + co * py + cy);
            }
            area[static_cast<std::size_t>(y) * w + x] = sum / 16;
        }
    luma.assign(static_cast<std::size_t>(w) * h, 0);
    const auto at = [&](int x, int y) { return area[static_cast<std::size_t>(std::clamp(y, 0, static_cast<int>(h) - 1)) * w + static_cast<unsigned>(std::clamp(x, 0, static_cast<int>(w) - 1))]; };
    for (int y = 0; y < static_cast<int>(h); ++y)
        for (int x = 0; x < static_cast<int>(w); ++x) {
            double sum = 0;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) sum += (j ? 1 : 2) * (i ? 1 : 2) * at(x + i, y + j);
            luma[static_cast<std::size_t>(y) * w + static_cast<unsigned>(x)] = static_cast<std::uint8_t>(std::clamp(std::lround(sum / 16), 0l, 255l));
        }
}
void TestBars() {
    struct Case { const char* name; unsigned rows, columns; };
    static constexpr Case kCases[]{{"letterbox 2.39:1", 92, 0}, {"pillarbox 4:3", 0, 160}};
    std::vector<std::uint8_t> luma;
    double worst = 0; unsigned found = 0, misled = 0;
    for (const auto& c : kCases)
        for (const double t : {4.5, -6.0}) {
            Draw(t, 1280, 720, luma); horizon_scene::Bars(luma, 1280, 720, c.rows, c.columns, 7);
            const auto m = MeasureCpu(luma, 1280, 720);
            std::uint32_t words[HorizonLevel::kWords]{};
            HorizonLevel::Accumulate(luma.data(), 1280, 1280, 720, words, 1, 0); // Black as code 0: the bars are not black to the bar test.
            const auto blind = HorizonLevel::Measure(words);
            std::printf("Bars, %s, %+.1f degrees: %+.2f (mass %.0f); without the bar test %s%+.2f\n", c.name, t, static_cast<double>(m.tilt), static_cast<double>(m.mass),
                        blind.found ? "" : "no tilt, ", static_cast<double>(blind.tilt));
            found += m.found; worst = (std::max)(worst, std::fabs(m.tilt - t));
            if (c.rows && (!blind.found || std::fabs(blind.tilt - t) >= 1.0)) ++misled; // The letterbox's edges: 160 blocks at exactly 0.
        }
    // The pillarbox's edges are vertical lines (half weight, 90 blocks): they do not outweigh the scene even unseen.
    std::printf("Bars: worst %.2f degree; without the bar test the letterbox misled %u of 2 times\n", worst, misled);
    CHECK(found == 4 && worst <= 0.35 && misled == 2);
}
void TestPerspective() {
    // A facade: window bands whose edges recede at `slope` degrees (horizontal lines in perspective), mullions standing vertical.
    const auto facade = [](double slope) {
        return [slope](double u, double v) {
            const double t = std::tan(slope * kPi / 180), along = v - t * (u - 640), band = along / 48 - std::floor(along / 48), mullion = u / 64 - std::floor(u / 64);
            return mullion < 0.18 ? 40.0 : band < 0.3 ? 200.0 : 120.0;
        };
    };
    std::vector<std::uint8_t> luma;
    Render(0, 1280, 720, luma, facade(7.0));
    const auto receding = MeasureCpu(luma, 1280, 720);
    Render(0, 1280, 720, luma, facade(0.0));
    const auto level = MeasureCpu(luma, 1280, 720);
    Render(4.5, 1280, 720, luma, facade(0.0));
    const auto rolled = MeasureCpu(luma, 1280, 720);
    std::printf("Perspective: receding at +7 (horizontal lines %+.2f, vertical %+.2f): %s; level: %s%+.2f; rolled +4.5: %s%+.2f\n",
                static_cast<double>(receding.lines[0]), static_cast<double>(receding.lines[1]), receding.perspective ? "perspective, no tilt" : receding.found ? "a tilt" : "no tilt",
                level.found ? "" : "not found ", static_cast<double>(level.tilt), rolled.found ? "" : "not found ", static_cast<double>(rolled.tilt));
    CHECK(receding.classes == 3 && receding.perspective && !receding.found);
    CHECK(level.found && std::fabs(level.tilt) <= 0.35 && rolled.found && std::fabs(rolled.tilt - 4.5) <= 0.35);
}
void TestMatte() {
    using media::video::Matte;
    constexpr unsigned W = 1280, H = 720, kPictures = 45;
    std::vector<std::uint8_t> base, luma(W * H);
    Draw(3.0, W, H, base);
    struct Case { const char* name; unsigned rows, columns; Matte::Rect truth; };
    static constexpr Case kCases[]{{"letterbox 2.39:1", 92, 0, {0, 92, W, H - 92}}, {"pillarbox 4:3", 0, 160, {160, 0, W - 160, H}}, {"no bars", 0, 0, {0, 0, W, H}}};
    std::uint64_t allocations = 0; bool good = true;
    const auto picture = [&](unsigned f, const Case& c) {
        for (unsigned i = 0; i < W * H; ++i) luma[i] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(base[i]) + static_cast<int>(Hash(i * 31u + f * 977u) % 7u) - 3, 0, 255));
        horizon_scene::Bars(luma, W, H, c.rows, c.columns, f + 1);
    };
    Matte letterbox;
    for (const auto& c : kCases) {
        Matte m; m.Reset(W, H); unsigned locked_at = 0;
        for (unsigned f = 0; f < kPictures; ++f) {
            picture(f, c);
            std::uint32_t words[Matte::kWords]{};
            allocation_probe::Begin();
            Matte::Accumulate(luma.data(), W, W, H, words);
            m.Push(static_cast<std::int64_t>(f) * 333333, words);
            allocations += allocation_probe::End();
            if (m.Locked() && !locked_at) locked_at = f + 1;
        }
        const auto r = m.Active();
        const auto close = [](std::uint32_t a, std::uint32_t b) { return (a > b ? a - b : b - a) <= 2; };
        const bool right = m.Locked() && close(r.left, c.truth.left) && close(r.top, c.truth.top) && close(r.right, c.truth.right) && close(r.bottom, c.truth.bottom) &&
                           m.Bars() == (c.rows || c.columns);
        std::printf("Matte, %s: locked after %u pictures; active (%u, %u)-(%u, %u) against (%u, %u)-(%u, %u); %s\n", c.name, locked_at, r.left, r.top, r.right, r.bottom,
                    c.truth.left, c.truth.top, c.truth.right, c.truth.bottom, m.Bars() ? "bars" : "no bars");
        good = good && right && locked_at >= 30 && locked_at <= 33;
        if (c.rows) letterbox = m;
    }
    // A burned-in subtitle in the letterbox's bottom bar (rows 650 to 669): the bar is thinner from that picture on.
    picture(kPictures, kCases[0]);
    for (unsigned y = 650; y < 670; ++y) for (unsigned x = 300; x < 980; ++x) luma[static_cast<std::size_t>(y) * W + x] = ((x / 6 + y / 4) % 3) ? 230 : 40;
    std::uint32_t words[Matte::kWords]{};
    Matte::Accumulate(luma.data(), W, W, H, words); letterbox.Push(static_cast<std::int64_t>(kPictures) * 333333, words);
    std::printf("Matte, a subtitle in the bottom bar: active bottom %u (the text ends at 670)\n", letterbox.Active().bottom);
    good = good && letterbox.Active().bottom >= 669 && letterbox.Active().bottom <= 670;
    // A dark sky (black to 18) over the scene's lower half, its one bright point (48 x 48) at another height in every picture.
    Matte dark; dark.Reset(W, H);
    for (unsigned f = 0; f < kPictures; ++f) {
        for (unsigned i = 0; i < W * H; ++i) luma[i] = i / W >= H / 2 ? base[i] : static_cast<std::uint8_t>(16 + Hash(i + f * 7u) % 3u);
        const unsigned sy = Hash(f * 131u + 5u) % (H / 2 - 48), sx = Hash(f * 17u + 3u) % (W - 48);
        for (unsigned y = sy; y < sy + 48; ++y) for (unsigned x = sx; x < sx + 48; ++x) luma[static_cast<std::size_t>(y) * W + x] = 220;
        std::uint32_t w[Matte::kWords]{};
        Matte::Accumulate(luma.data(), W, W, H, w); dark.Push(static_cast<std::int64_t>(f) * 333333, w);
    }
    std::printf("Matte, a dark sky with a point moving in it: %s, active top %u\n", dark.Bars() ? "BARS" : "no bars", dark.Active().top);
    good = good && !dark.Bars() && dark.Active().top == 0;
    std::printf("Matte: %llu allocations\n", static_cast<unsigned long long>(allocations));
    CHECK(good && allocations == 0);
}

// The CPU survey before 2026-10-31, kept here as the reference: the histograms in float (the shader's arithmetic, as it was in
// HorizonLevel::Accumulate) and the extents read at every even row and column (Matte::Accumulate).
void OldHorizon(const std::uint8_t* luma, unsigned stride, unsigned width, unsigned height, std::uint32_t* words, unsigned black = 16) {
    const float dark = static_cast<float>(black) / 255.0f + HorizonLevel::kLift;
    const unsigned s = (std::max)(8u, (width / 80 + 4) / 8 * 8);
    for (unsigned by = 0; by < height / s; ++by)
        for (unsigned bx = 0; bx < width / s; ++bx) {
            const int ox = static_cast<int>(bx * s + s / 2) - 5, oy = static_cast<int>(by * s + s / 2) - 5;
            if (ox < 0 || oy < 0 || ox + 9 > static_cast<int>(width) - 1 || oy + 9 > static_cast<int>(height) - 1) continue;
            float v[10][10];
            for (int j = 0; j < 10; ++j)
                for (int i = 0; i < 10; ++i) v[j][i] = static_cast<float>(luma[static_cast<std::size_t>(oy + j) * stride + static_cast<std::size_t>(ox + i)]) / 255.0f;
            float jxx = 0, jyy = 0, jxy = 0;
            for (int y = 1; y < 9; ++y)
                for (int x = 1; x < 9; ++x) {
                    const float gx = (3 * (v[y - 1][x + 1] - v[y - 1][x - 1]) + 10 * (v[y][x + 1] - v[y][x - 1]) + 3 * (v[y + 1][x + 1] - v[y + 1][x - 1])) / 32.0f;
                    const float gy = (3 * (v[y + 1][x - 1] - v[y - 1][x - 1]) + 10 * (v[y + 1][x] - v[y - 1][x]) + 3 * (v[y + 1][x + 1] - v[y - 1][x + 1])) / 32.0f;
                    jxx += gx * gx; jyy += gy * gy; jxy += gx * gy;
                }
            const float energy = jxx + jyy, d = jxx - jyy;
            if (energy < 0.005f) continue;
            const float coherence = std::sqrt(d * d + 4 * jxy * jxy) / energy;
            if (coherence < 0.85f) continue;
            float angle = 0.5f * std::atan2(2 * jxy, d) * 57.2957795f;
            unsigned lines = 1;
            if (std::fabs(angle) > 45) { angle -= angle > 0 ? 90.0f : -90.0f; lines = 0; }
            if (std::fabs(angle) < HorizonLevel::kBarAxis) {
                bool first = true, last = true;
                for (int i = 0; i < 10; ++i) { first = first && (lines ? v[i][0] : v[0][i]) <= dark; last = last && (lines ? v[i][9] : v[9][i]) <= dark; }
                if (first || last) continue;
            }
            const float at = (angle + 16.0f) * 2.0f;
            if (at < 0 || at >= static_cast<float>(HorizonLevel::kBins)) continue;
            const unsigned bin = (std::min)(static_cast<unsigned>(at), HorizonLevel::kBins - 1), weight = static_cast<unsigned>(16.0f * coherence * coherence + 0.5f);
            const unsigned fraction = (std::min)(static_cast<unsigned>((at - static_cast<float>(bin)) * 256.0f), 255u);
            words[2 * (lines * HorizonLevel::kBins + bin)] += weight; words[2 * (lines * HorizonLevel::kBins + bin) + 1] += weight * fraction;
        }
}
void OldMatte(const std::uint8_t* luma, unsigned stride, unsigned width, unsigned height, std::uint32_t* words, unsigned black = 16) {
    using media::video::Matte;
    const float threshold = static_cast<float>(black) / 255.0f + Matte::kLift;
    const auto bright = [&](unsigned x, unsigned y) { return static_cast<float>(luma[static_cast<std::size_t>(y) * stride + x]) / 255.0f > threshold; };
    const unsigned columns = Matte::Lines(width), rows = Matte::Lines(height);
    for (unsigned y = 0; y < height; y += 2)
        for (unsigned i = 0; i < columns; ++i)
            if (bright(Matte::Sample(i, columns, width), y)) { words[0] = (std::max)(words[0], 65536u - y); words[1] = (std::max)(words[1], y + 1); break; }
    for (unsigned x = 0; x < width; x += 2)
        for (unsigned j = 0; j < rows; ++j)
            if (bright(x, Matte::Sample(j, rows, height))) { words[2] = (std::max)(words[2], 65536u - x); words[3] = (std::max)(words[3], x + 1); break; }
}
// The CPU survey (docs/picture.md, "The CPU survey", 2026-10-31): what the SDK's host mode does per picture it holds.
// - Every vector level (scalar, SSE2, AVX2, as available) gives the same histogram words, bit for bit, on pictures of every
//   kind and size (odd widths: the blocks' tails; an RGBA channel, step 4, is scalar and gives the words of the same luma);
// - the extents are the old loops' words exactly (letterbox, pillarbox, windowbox, a subtitle in a bar, noise, black);
// - the histograms measure what the float ones did (found the same, tilt within 0.02 degree);
// - the survey's time (histograms, extents, measure) per level at 640x480 and 1280x720 against the old: at most 0.25 ms at
//   640x480 (Release, SSE2 or better; not under a sanitizer); no allocation.
void TestSurvey() {
    using media::video::Matte;
    struct Picture { const char* name; unsigned w, h; std::vector<std::uint8_t> luma; };
    std::vector<Picture> pictures;
    const auto add = [&](const char* name, unsigned w, unsigned h, double tilt, unsigned rows, unsigned columns) {
        Picture p{name, w, h, {}}; Draw(tilt, w, h, p.luma);
        if (rows || columns) horizon_scene::Bars(p.luma, w, h, rows, columns, 11);
        pictures.push_back(std::move(p));
    };
    add("640x480 +4.5", 640, 480, 4.5, 0, 0);
    add("640x480 -6.0 letterbox", 640, 480, -6.0, 66, 0);
    add("1280x720 +12 pillarbox", 1280, 720, 12.0, 0, 160);
    add("1280x720 -14 windowbox", 1280, 720, -14.0, 40, 100);
    add("641x479 +3", 641, 479, 3.0, 0, 0);
    add("1920x1080 -2", 1920, 1080, -2.0, 0, 0);
    add("200x130 +1", 200, 130, 1.0, 0, 0);
    {
        Picture p{"noise 640x480", 640, 480, std::vector<std::uint8_t>(640 * 480)};
        for (unsigned i = 0; i < 640 * 480; ++i) p.luma[i] = static_cast<std::uint8_t>(Hash(i * 7u + 3u));
        pictures.push_back(std::move(p));
        Picture b{"black 640x480", 640, 480, std::vector<std::uint8_t>(640 * 480, 16)};
        pictures.push_back(std::move(b));
        Picture t{"subtitle in the bar", 640, 480, {}}; Draw(4.5, 640, 480, t.luma); horizon_scene::Bars(t.luma, 640, 480, 60, 0, 3);
        for (unsigned y = 440; y < 452; ++y) for (unsigned x = 200; x < 420; ++x) t.luma[static_cast<std::size_t>(y) * 640 + x] = (x / 3 % 2) ? 220 : 30;
        pictures.push_back(std::move(t));
    }
    const unsigned available = HorizonLevel::ForceLevel(2);
    unsigned differing = 0, old_differing = 0, matte_differing = 0, measured = 0;
    double worst = 0;
    for (const auto& p : pictures) {
        std::uint32_t words[3][HorizonLevel::kWords]{};
        for (unsigned level = 0; level <= available; ++level) {
            HorizonLevel::ForceLevel(level);
            HorizonLevel::Accumulate(p.luma.data(), p.w, p.w, p.h, words[level]);
            if (std::memcmp(words[level], words[0], sizeof(words[0])) != 0) ++differing;
        }
        // An RGBA channel (step 4): the same words.
        std::vector<std::uint8_t> rgba(std::size_t{p.w} * p.h * 4);
        for (std::size_t i = 0; i < std::size_t{p.w} * p.h; ++i) { rgba[4 * i] = static_cast<std::uint8_t>(i); rgba[4 * i + 1] = p.luma[i]; rgba[4 * i + 2] = 99; rgba[4 * i + 3] = 255; }
        std::uint32_t channel[HorizonLevel::kWords]{};
        HorizonLevel::Accumulate(rgba.data() + 1, p.w * 4, p.w, p.h, channel, 4);
        if (std::memcmp(channel, words[0], sizeof(channel)) != 0) ++differing;
        std::uint32_t old[HorizonLevel::kWords]{};
        OldHorizon(p.luma.data(), p.w, p.w, p.h, old);
        const auto now = HorizonLevel::Measure(words[0]), before = HorizonLevel::Measure(old);
        if (now.found != before.found || now.perspective != before.perspective) ++old_differing;
        if (now.found && before.found) { ++measured; worst = (std::max)(worst, static_cast<double>(std::fabs(now.tilt - before.tilt))); }
        std::uint32_t e[Matte::kWords]{}, f[Matte::kWords]{}, g[Matte::kWords]{}, h[Matte::kWords]{};
        Matte::Accumulate(p.luma.data(), p.w, p.w, p.h, e); OldMatte(p.luma.data(), p.w, p.w, p.h, f);
        Matte::Accumulate(rgba.data() + 1, p.w * 4, p.w, p.h, g, 16, 4);
        Matte::Accumulate(p.luma.data(), p.w, p.w, p.h, h, 0); // Full range: black is code 0.
        std::uint32_t h_old[Matte::kWords]{}; OldMatte(p.luma.data(), p.w, p.w, p.h, h_old, 0);
        if (std::memcmp(e, f, sizeof(e)) != 0 || std::memcmp(g, f, sizeof(g)) != 0 || std::memcmp(h, h_old, sizeof(h)) != 0) ++matte_differing;
        std::printf("Survey, %s: %s%+.3f (float %s%+.3f); extents %u %u %u %u\n", p.name, now.found ? "" : "no tilt ", static_cast<double>(now.tilt),
                    before.found ? "" : "no tilt ", static_cast<double>(before.tilt), e[0], e[1], e[2], e[3]);
    }
    std::printf("Survey: levels 0..%u, %zu pictures: %u histograms differing; float reference: %u verdicts differing, %u tilts within %.3f; extents %u differing\n",
                available, pictures.size(), differing, old_differing, measured, worst, matte_differing);
    CHECK(differing == 0 && old_differing == 0 && worst <= 0.02 && matte_differing == 0);
    // The time per picture: histograms + extents + measure, best of 25, per level; the old loops for comparison.
    std::uint64_t allocations = 0; double best640 = 1e9;
    for (const auto& size : {std::pair<unsigned, unsigned>{640, 480}, std::pair<unsigned, unsigned>{1280, 720}}) {
        std::vector<std::uint8_t> p; Draw(4.5, size.first, size.second, p);
        const auto time = [&](int level) {
            double best = 1e9;
            for (unsigned k = 0; k < 25; ++k) {
                if (level >= 0) HorizonLevel::ForceLevel(static_cast<unsigned>(level));
                std::uint32_t h[HorizonLevel::kWords]{}, e[Matte::kWords]{};
                if (level >= 0) allocation_probe::Begin();
                const auto start = std::chrono::steady_clock::now();
                if (level >= 0) { HorizonLevel::Accumulate(p.data(), size.first, size.first, size.second, h); Matte::Accumulate(p.data(), size.first, size.first, size.second, e); }
                else { OldHorizon(p.data(), size.first, size.first, size.second, h); OldMatte(p.data(), size.first, size.first, size.second, e); }
                const auto m = HorizonLevel::Measure(h);
                best = (std::min)(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                if (level >= 0) allocations += allocation_probe::End();
                CHECK(m.found && std::fabs(m.tilt - 4.5f) <= 0.35f);
            }
            return best;
        };
        const double old = time(-1);
        std::printf("Survey on the CPU (histograms + extents + measure), %ux%u, best of 25: before %.3f ms; scalar %.3f", size.first, size.second, old, time(0));
        for (unsigned level = 1; level <= available; ++level) std::printf(", %s %.3f", level == 1 ? "SSE2" : "AVX2", time(static_cast<int>(level)));
        std::printf(" ms\n");
        if (size.first == 640) best640 = time(static_cast<int>(available));
    }
    HorizonLevel::ForceLevel(available);
    std::printf("Survey: %llu allocations\n", static_cast<unsigned long long>(allocations));
    CHECK(allocations == 0);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    if (available >= 1) CHECK(best640 <= 0.25);
#else
    (void)best640;
#endif
}

#if defined(_WIN32)
template <class T> void ReleaseCom(T*& p) { if (p) { p->Release(); p = nullptr; } }
using media::render::ColorConstants;
using media::render::ColorPipeline;
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to}; return b;
}
struct Gpu {
    ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr; ID3D12QueryHeap* queries = nullptr;
    ID3D12DescriptorHeap* heap = nullptr; UINT stride = 0; std::uint64_t value = 0, frequency = 0;
    ColorPipeline color;
    bool Create() {
        IDXGIFactory6* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
            for (UINT i = 0; !device; ++i) {
                IDXGIAdapter1* adapter = nullptr;
                if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) (void)D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
                ReleaseCom(adapter);
            }
        }
        ReleaseCom(factory);
        if (!device) return false;
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = color.Create(device);
        D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap));
        D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
        if (SUCCEEDED(hr)) hr = device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries));
        if (SUCCEEDED(hr)) hr = queue->GetTimestampFrequency(&frequency);
        stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return SUCCEEDED(hr);
    }
    bool Submit() {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence, ++value))) return false;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool waited = SUCCEEDED(fence->SetEventOnCompletion(value, event)) && WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
        CloseHandle(event);
        return waited && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr));
    }
    ID3D12Resource* Buffer(D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state) const {
        D3D12_HEAP_PROPERTIES p{}; p.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = nullptr;
        return SUCCEEDED(device->CreateCommittedResource(&p, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))) ? r : nullptr;
    }
    ID3D12Resource* Texture(unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) const {
        D3D12_HEAP_PROPERTIES p{}; p.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = format; d.SampleDesc.Count = 1; d.Flags = flags;
        ID3D12Resource* r = nullptr;
        return SUCCEEDED(device->CreateCommittedResource(&p, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r))) ? r : nullptr;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned i) const { auto x = heap->GetCPUDescriptorHandleForHeapStart(); x.ptr += static_cast<SIZE_T>(i) * stride; return x; }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu_(unsigned i) const { auto x = heap->GetGPUDescriptorHandleForHeapStart(); x.ptr += static_cast<UINT64>(i) * stride; return x; }
    ~Gpu() { color.Release(); ReleaseCom(queries); ReleaseCom(heap); ReleaseCom(list); ReleaseCom(allocator); ReleaseCom(fence); ReleaseCom(queue); ReleaseCom(device); }
};
// One picture size: the NV12 source, the RGBA output, their upload and readback buffers.
struct Picture {
    Gpu& gpu; unsigned w, h;
    ID3D12Resource *source = nullptr, *output = nullptr, *upload = nullptr, *image = nullptr, *stats = nullptr, *horizon = nullptr, *times = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT planes[2]{}, rgba{};
    Picture(Gpu& g, unsigned width, unsigned height) : gpu(g), w(width), h(height) {}
    bool Create() {
        source = gpu.Texture(w, h, DXGI_FORMAT_NV12, D3D12_RESOURCE_FLAG_NONE);
        output = gpu.Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!source || !output) return false;
        const auto desc = source->GetDesc(); UINT rows[2]{}; UINT64 sizes[2]{}, total = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 2, 0, planes, rows, sizes, &total);
        upload = gpu.Buffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        const auto out = output->GetDesc(); UINT64 bytes = 0;
        gpu.device->GetCopyableFootprints(&out, 0, 1, 0, &rgba, nullptr, nullptr, &bytes);
        image = gpu.Buffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        stats = gpu.Buffer(D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
        horizon = gpu.Buffer(D3D12_HEAP_TYPE_READBACK, ColorPipeline::kHorizonBytes, D3D12_RESOURCE_STATE_COPY_DEST);
        times = gpu.Buffer(D3D12_HEAP_TYPE_READBACK, 32, D3D12_RESOURCE_STATE_COPY_DEST);
        return upload && image && stats && horizon && times;
    }
    bool Upload(const std::vector<std::uint8_t>& luma) {
        std::uint8_t* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        for (unsigned r = 0; r < h; ++r) std::memcpy(mapped + planes[0].Offset + r * planes[0].Footprint.RowPitch, luma.data() + static_cast<std::size_t>(r) * w, w);
        for (unsigned r = 0; r < h / 2; ++r) std::memset(mapped + planes[1].Offset + r * planes[1].Footprint.RowPitch, 128, w);
        upload->Unmap(0, nullptr);
        auto b = Transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST); gpu.list->ResourceBarrier(1, &b);
        for (unsigned s = 0; s < 2; ++s) {
            D3D12_TEXTURE_COPY_LOCATION to{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; to.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION from{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = planes[s];
            gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        b = Transition(source, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON); gpu.list->ResourceBarrier(1, &b);
        return gpu.Submit();
    }
    // Converts the uploaded picture with `c` and the statistics pass; the histograms into `words` (when given), the output's
    // luma into `out` (when given), the statistics pass's GPU time into `stats_ms`.
    bool Convert(const ColorConstants& c, std::uint32_t* words, std::vector<std::uint8_t>* out, double* stats_ms = nullptr, std::uint32_t* statistics = nullptr) {
        for (unsigned plane = 0; plane < 2; ++plane) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
            gpu.device->CreateShaderResourceView(source, &srv, gpu.Cpu(plane));
        }
        gpu.device->CreateUnorderedAccessView(output, nullptr, nullptr, gpu.Cpu(2));
        D3D12_RESOURCE_BARRIER two[]{Transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                     Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        gpu.list->ResourceBarrier(2, two);
        ID3D12DescriptorHeap* heaps[]{gpu.heap}; gpu.list->SetDescriptorHeaps(1, heaps);
        gpu.color.Record(gpu.list, gpu.Gpu_(0), gpu.Gpu_(2), c);
        gpu.list->EndQuery(gpu.queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        gpu.color.RecordStats(gpu.list, c, stats, 0, horizon, 0);
        gpu.list->EndQuery(gpu.queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        gpu.list->ResolveQueryData(gpu.queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, times, 0);
        two[0] = Transition(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        two[1] = Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->ResourceBarrier(2, two);
        if (out) {
            D3D12_TEXTURE_COPY_LOCATION to{image, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = rgba;
            D3D12_TEXTURE_COPY_LOCATION from{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        auto b = Transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON); gpu.list->ResourceBarrier(1, &b);
        if (!gpu.Submit()) return false;
        const D3D12_RANGE none{0, 0};
        std::uint8_t* mapped = nullptr;
        if (words) {
            if (FAILED(horizon->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
            std::memcpy(words, mapped, ColorPipeline::kHorizonBytes); horizon->Unmap(0, &none);
        }
        if (statistics) { // [8, 64) of the statistics: the extents are words 10 to 13.
            if (FAILED(stats->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
            std::memcpy(statistics, mapped, ColorPipeline::kStatsBytes); stats->Unmap(0, &none);
        }
        if (stats_ms) {
            if (FAILED(times->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
            std::uint64_t t[2]; std::memcpy(t, mapped, sizeof(t)); times->Unmap(0, &none);
            *stats_ms = static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(gpu.frequency);
        }
        if (out) {
            if (FAILED(image->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
            out->assign(static_cast<std::size_t>(w) * h, 0);
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    const auto* q = mapped + rgba.Offset + static_cast<std::size_t>(y) * rgba.Footprint.RowPitch + 4 * x;
                    (*out)[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(std::lround(0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2]));
                }
            image->Unmap(0, &none);
        }
        return true;
    }
    ~Picture() { ReleaseCom(times); ReleaseCom(horizon); ReleaseCom(stats); ReleaseCom(image); ReleaseCom(upload); ReleaseCom(output); ReleaseCom(source); }
};
ColorConstants Plain(unsigned w, unsigned h) { // BT.709 limited range, 8-bit, SDR, 1:1, no reconstruction.
    ColorConstants c{w, h, w, h, 0, 0, w, h, 1, 0, 0, 1, 1, 0, 1000.0f, 203.0f, 0, 0.0f, 0.02f, 0, 0.0f, 0, 0};
    c.contrast = 1; c.saturation = 1; c.gamma = 1; c.micro_gain = 1;
    return c;
}
void TestGpu() {
    Gpu gpu;
    if (!gpu.Create()) { std::puts("GPU: no hardware D3D12 device, skipped"); return; }
    constexpr unsigned W = 1280, H = 720;
    Picture p(gpu, W, H);
    CHECK(p.Create());
    const double tilts[]{4.5, -6.0};
    for (const double truth : tilts) {
        std::vector<std::uint8_t> luma, out;
        Draw(truth, W, H, luma);
        if (!p.Upload(luma)) { CHECK(!"upload"); return; }
        auto c = Plain(W, H); c.filter_flags = media::render::kHorizon;
        std::uint32_t gpu_words[HorizonLevel::kWords]{}, cpu_words[HorizonLevel::kWords]{};
        CHECK(p.Convert(c, gpu_words, nullptr));
        HorizonLevel::Accumulate(luma.data(), W, W, H, cpu_words);
        const auto m = HorizonLevel::Measure(gpu_words), reference = HorizonLevel::Measure(cpu_words);
        double totals[2][2]{}; // [gpu, cpu][class]
        for (unsigned k = 0; k < 2; ++k)
            for (unsigned b = 0; b < HorizonLevel::kBins; ++b) { totals[0][k] += gpu_words[2 * (k * HorizonLevel::kBins + b)]; totals[1][k] += cpu_words[2 * (k * HorizonLevel::kBins + b)]; }
        // The leveling turn the renderer applies (level = minus the tilt), and the output's own lines measured on the CPU.
        media::video::Orientation o; o.level = -m.tilt;
        double mm[4], t[2]; o.Map(W, H, W, H, mm, t);
        auto turned = Plain(W, H); turned.filter_flags = media::render::kRotate;
        for (unsigned i = 0; i < 4; ++i) turned.view_m[i] = static_cast<float>(mm[i]);
        turned.view_t[0] = static_cast<float>(t[0]); turned.view_t[1] = static_cast<float>(t[1]);
        CHECK(p.Convert(turned, nullptr, &out));
        const auto leveled = MeasureCpu(out, W, H);
        std::printf("GPU %+.1f degrees: the histograms measure %+.2f (mass %.0f; the CPU reference %+.2f), weights GPU / CPU: lines near horizontal %.0f / %.0f, "
                    "near vertical %.0f / %.0f; leveled by %+.2f, the output measures %+.2f\n", truth, static_cast<double>(m.tilt), static_cast<double>(m.mass),
                    static_cast<double>(reference.tilt), totals[0][0], totals[1][0], totals[0][1], totals[1][1], static_cast<double>(o.level), static_cast<double>(leveled.tilt));
        CHECK(m.found && std::fabs(m.tilt - truth) <= 0.35 && reference.found);
        for (unsigned k = 0; k < 2; ++k) CHECK(std::fabs(totals[0][k] - totals[1][k]) <= 0.03 * totals[1][k]);
        CHECK(leveled.found && std::fabs(leveled.tilt) <= 0.35);
    }
    // Black bars: the letterboxed and pillarboxed pictures' tilt from the GPU's histograms, and the extents against the CPU's words.
    for (const auto& bars : {std::pair<unsigned, unsigned>{92, 0}, std::pair<unsigned, unsigned>{0, 160}})
        for (const double truth : tilts) {
            std::vector<std::uint8_t> luma;
            Draw(truth, W, H, luma); horizon_scene::Bars(luma, W, H, bars.first, bars.second, 3);
            if (!p.Upload(luma)) { CHECK(!"upload"); return; }
            auto c = Plain(W, H); c.filter_flags = media::render::kHorizon | media::render::kMatte;
            std::uint32_t words[HorizonLevel::kWords]{}, statistics[ColorPipeline::kStatsBytes / 4]{}, cpu[media::video::Matte::kWords]{};
            CHECK(p.Convert(c, words, nullptr, nullptr, statistics));
            media::video::Matte::Accumulate(luma.data(), W, W, H, cpu);
            const auto m = HorizonLevel::Measure(words);
            media::video::Matte::Rect e{}; const bool extents = media::video::Matte::Extents(statistics + 10, W, H, e);
            const bool same = std::memcmp(statistics + 10, cpu, sizeof(cpu)) == 0;
            std::printf("GPU %s %+.1f degrees: the histograms measure %+.2f; the extents (%u, %u)-(%u, %u), %s the CPU's\n", bars.first ? "letterbox" : "pillarbox", truth,
                        static_cast<double>(m.tilt), e.left, e.top, e.right, e.bottom, same ? "the same as" : "NOT");
            CHECK(m.found && std::fabs(m.tilt - truth) <= 0.35 && extents && same);
        }
    // The statistics pass's cost at 1280x720 and at 3840x2160 (the 1280x720 picture tiled): alone, with the extents, with the
    // extents and the histograms: medians of 15.
    Picture big(gpu, 3840, 2160);
    CHECK(big.Create());
    std::vector<std::uint8_t> picture, tiled(3840u * 2160u);
    Draw(4.5, W, H, picture);
    for (unsigned y = 0; y < 2160; ++y) for (unsigned x = 0; x < 3840; ++x) tiled[static_cast<std::size_t>(y) * 3840 + x] = picture[static_cast<std::size_t>(y % H) * W + x % W];
    if (!big.Upload(tiled) || !p.Upload(picture)) { CHECK(!"upload"); return; }
    double ms[2][3][15]{}; // [size][alone, extents, extents and histograms]
    static constexpr std::uint32_t kFlags[]{0u, media::render::kMatte, media::render::kMatte | media::render::kHorizon};
    for (unsigned i = 0; i < 15; ++i)
        for (unsigned size = 0; size < 2; ++size)
            for (unsigned with = 0; with < 3; ++with) {
                auto c = size ? Plain(3840, 2160) : Plain(W, H); c.filter_flags = kFlags[with];
                CHECK((size ? big : p).Convert(c, nullptr, nullptr, &ms[size][with][i]));
            }
    for (auto& size : ms) for (auto& m : size) std::sort(m, m + 15);
    std::printf("GPU statistics pass (medians of 15): 1280x720 %.3f ms alone, %.3f with the extents, %.3f with the histograms too; 3840x2160 %.3f / %.3f / %.3f ms\n",
                ms[0][0][7], ms[0][1][7], ms[0][2][7], ms[1][0][7], ms[1][1][7], ms[1][2][7]);
}

// Real footage: Media Foundation's source reader, pictures as NV12 (their luma).
struct Reader {
    IMFSourceReader* reader = nullptr; unsigned width = 0, height = 0, stride = 0;
    bool Open(const wchar_t* path) {
        IMFAttributes* attributes = nullptr;
        if (FAILED(MFCreateAttributes(&attributes, 1))) return false;
        (void)attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        const bool made = SUCCEEDED(MFCreateSourceReaderFromURL(path, attributes, &reader));
        ReleaseCom(attributes);
        if (!made) return false;
        const auto stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        (void)reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE); (void)reader->SetStreamSelection(stream, TRUE);
        IMFMediaType* type = nullptr;
        if (FAILED(MFCreateMediaType(&type))) return false;
        (void)type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); (void)type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        const bool set = SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, type));
        ReleaseCom(type);
        if (!set || FAILED(reader->GetCurrentMediaType(stream, &type))) return false;
        UINT32 w = 0, h = 0; (void)MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h); width = w; height = h;
        stride = static_cast<unsigned>(MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, w));
        ReleaseCom(type);
        return width && height;
    }
    bool Next(std::vector<std::uint8_t>& luma) { // The picture's luma, width x height (stride width).
        for (;;) {
            DWORD flags = 0; LONGLONG time = 0; IMFSample* sample = nullptr;
            if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &time, &sample)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                ReleaseCom(sample); return false;
            }
            if (!sample) continue;
            IMFMediaBuffer* buffer = nullptr; BYTE* data = nullptr; DWORD length = 0;
            const bool ok = SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length)) && length >= stride * height;
            if (ok) { luma.resize(static_cast<std::size_t>(width) * height); for (unsigned y = 0; y < height; ++y) std::memcpy(luma.data() + static_cast<std::size_t>(y) * width, data + static_cast<std::size_t>(y) * stride, width); }
            if (buffer && data) buffer->Unlock();
            ReleaseCom(buffer); ReleaseCom(sample);
            return ok;
        }
    }
    ~Reader() { ReleaseCom(reader); }
};
// A cw x ch window of `source` (w x h) about its centre, turned clockwise by `roll` degrees (the scene as a rolled camera sees
// it: output p shows source R(-roll) (p - c) + c, bilinear), half a sample off the source's grid so that the unturned window is
// interpolated like the turned ones (sampled on the grid it read 0.1 to 0.3 degree differently: the footage's lines are a spread
// of the wet sand's orientations, which interpolation's blur reweights).
void Turned(const std::vector<std::uint8_t>& source, unsigned w, unsigned h, double roll, unsigned cw, unsigned ch, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(cw) * ch, 0);
    const double r = roll * kPi / 180, co = std::cos(r), si = std::sin(r);
    for (unsigned y = 0; y < ch; ++y)
        for (unsigned x = 0; x < cw; ++x) {
            const double px = x + 0.5 - cw / 2.0, py = y + 0.5 - ch / 2.0;
            const double sx = co * px + si * py + w / 2.0, sy = -si * px + co * py + h / 2.0; // Half a sample off the grid: unturned, resampled alike.
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, static_cast<int>(w) - 2), y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, static_cast<int>(h) - 2);
            const double fx = std::clamp(sx - x0, 0.0, 1.0), fy = std::clamp(sy - y0, 0.0, 1.0);
            const auto at = [&](int i, int j) { return static_cast<double>(source[static_cast<std::size_t>(y0 + j) * w + static_cast<unsigned>(x0 + i)]); };
            out[static_cast<std::size_t>(y) * cw + x] = static_cast<std::uint8_t>(std::lround((1 - fy) * ((1 - fx) * at(0, 0) + fx * at(1, 0)) + fy * ((1 - fx) * at(0, 1) + fx * at(1, 1))));
        }
}
double Median(std::vector<double> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; }
void TestFootage(const wchar_t* directory) {
    wchar_t path[2048];
    swprintf(path, 2048, L"%s\\costa_rica_4k60_hdr.mp4", directory);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { std::puts("Footage: costa_rica_4k60_hdr.mp4 not present, skipped"); return; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION))) { CHECK(!"Media Foundation"); return; }
    {
        Reader video;
        if (!video.Open(path)) { CHECK(!"the footage could not be decoded"); MFShutdown(); CoUninitialize(); return; }
        constexpr unsigned CW = 960, CH = 540, kPictures = 60, kRows = 69; // 2.39:1 inside 960x540.
        static constexpr double kRolls[]{0.0, 4.5, -6.0};
        std::vector<std::uint8_t> frame, window, gpu_window;
        std::vector<double> tilts[3][2];        // [roll][letterboxed]
        HorizonLevel level[3][2];
        for (unsigned f = 0; f < 2 * kPictures && video.Next(frame); ++f) {
            if (f % 2) continue; // Every other picture: 60 over 2 s.
            for (unsigned r = 0; r < 3; ++r)
                for (unsigned boxed = 0; boxed < 2; ++boxed) {
                    Turned(frame, video.width, video.height, kRolls[r], CW, CH, window);
                    if (boxed) horizon_scene::Bars(window, CW, CH, kRows, 0, f + 1);
                    const auto m = MeasureCpu(window, CW, CH);
                    level[r][boxed].Push(static_cast<std::int64_t>(f) * 166833, m);
                    if (m.found) tilts[r][boxed].push_back(m.tilt);
                    if (f == 40 && r == 1 && boxed) gpu_window = window;
                }
        }
        // Against the same window unturned (and boxed alike): its median tilt and the estimator's level (the last 15 pictures'
        // median) plus the roll. The footage's own lines are a spread of the wet sand's orientations around the shore's, not one
        // horizon: within 0.5 degree here (the synthetic scenes are held to 0.35).
        const double own = Median(tilts[0][0]);
        std::printf("Footage: %ux%u, a 960x540 window of 60 pictures; its own tilt %+.2f degrees (median of %zu measured)\n", video.width, video.height, own, tilts[0][0].size());
        bool good = tilts[0][0].size() >= 30 && std::fabs(own) <= 2.0;
        double worst = 0;
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned boxed = 0; boxed < 2; ++boxed) {
                const double truth = Median(tilts[0][boxed]) + kRolls[r], median = Median(tilts[r][boxed]);
                const auto& l = level[r][boxed];
                const double expected = static_cast<double>(level[0][boxed].Target()) + kRolls[r];
                std::printf("Footage turned %+.1f%s: measured on %zu of 60, median %+.2f against %+.2f; the level's estimate %+.2f (%s) against %+.2f\n", kRolls[r],
                            boxed ? ", letterboxed" : "", tilts[r][boxed].size(), median, truth, static_cast<double>(l.Target()), l.Locked() ? "locked" : "not locked", expected);
                good = good && tilts[r][boxed].size() >= 30 && l.Locked();
                worst = (std::max)({worst, std::fabs(median - truth), std::fabs(l.Target() - expected)});
            }
        std::printf("Footage: worst %.2f degree from the roll\n", worst);
        CHECK(good && worst <= 0.5);
        // One turned, letterboxed picture through the GPU against the CPU's measurement of it.
        Gpu gpu;
        if (!gpu_window.empty() && gpu.Create()) {
            Picture p(gpu, CW, CH);
            if (p.Create() && p.Upload(gpu_window)) {
                auto c = Plain(CW, CH); c.filter_flags = media::render::kHorizon;
                std::uint32_t words[HorizonLevel::kWords]{};
                CHECK(p.Convert(c, words, nullptr));
                const auto g = HorizonLevel::Measure(words), cpu = MeasureCpu(gpu_window, CW, CH);
                std::printf("Footage on the GPU (turned +4.5, letterboxed): %+.2f, the CPU %+.2f\n", static_cast<double>(g.tilt), static_cast<double>(cpu.tilt));
                CHECK(g.found && cpu.found && std::fabs(g.tilt - cpu.tilt) <= 0.1);
            }
        }
    }
    MFShutdown(); CoUninitialize();
}
#endif
}

int main(int argc, char** argv) {
    TestStill();
    TestSequence();
    TestBars();
    TestPerspective();
    TestMatte();
    TestSurvey();
#if defined(_WIN32)
    TestGpu();
    if (argc > 1) { wchar_t directory[1024]{}; MultiByteToWideChar(CP_ACP, 0, argv[1], -1, directory, 1024); TestFootage(directory); }
#else
    (void)argc; (void)argv;
#endif
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "auto horizon tests passed", failures);
    return failures ? 1 : 0;
}
