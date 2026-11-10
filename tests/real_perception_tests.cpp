// Perception and sound localisation on real footage (docs/picture.md, "Perception on real footage"; docs/audio-dsp.md,
// "Reverberant rooms"): costa_rica_4k60_hdr.mp4 (720p59.94 H.264 nature footage and its AAC soundtrack), decoded by Media Foundation
// for this test only (the engine decodes on the GPU and never brings pictures back), its luma fed to video::Perception:
// - A, the footage as it is (a low camera on a beach, moving slowly forward, water washing toward it): the tiles' dominant
//   motion against an independent measurement, the whole picture's translation found by SAD at full resolution between
//   consecutive pictures (where that registration is unambiguous); how much of the picture the flow covers, how much is
//   salient; how often the beach in the foreground is called looming: it is the ground the camera travels over (2026-10-27:
//   the ground's flow model takes it out; it was 91 of 292 pictures, now at most 20), and on how many pictures the ground is
//   found;
// - B, a hand-held camera's shake added (a 960x540 window moving +-12 / 9 px at 7 / 9 Hz over the 1280x720 pictures, the
//   IMU's deltas from that motion): the dominant motion, looming and saliency the same as for the still window;
// - C, an obstacle approaching at 1 m/s from 3 m pasted onto the footage (a textured square growing as Z0 / Z): on the
//   pictures where it is the strongest approach (its time to contact under 0.8x the footage's own, from A) and within 2 s of
//   contact, looming flagged, the time to contact against the truth, the hazard on it; and from 2 to 2.8 s (under 1/s of
//   expansion over moving water, two coarse tiles wide), found on at least 80 % of those pictures (2026-10-27: the fovea's
//   finer tiles; 0 of 15 before), its time to contact within 15 % (median).
// - D, pitched and cropped (2026-10-28): 960x540 windows of the footage with the horizon low in them (a camera pitched up:
//   the window at the top), high (pitched down: at the bottom), and a window pitching slowly (+-80 px at 0.3 Hz, the IMU's
//   pitch deltas from it): the ground found wherever the horizon is (the most rows from the bottom that one ground explains;
//   the IMU's pitch moving the last one), and the beach's false looming. Against the 2026-10-27 model (the bottom six rows):
//   ground on 45 pictures where it found none (horizon low), as often with the horizon high (200) and more often pitching
//   (135 against 121); looming on 10, 31 and 13 pictures against 10, 55 and 21.
// Audio: 30 ms noise bursts in a reverberant room (RT60 0.5 s: early reflections and a decorrelated diffuse tail), at four
// directions, as a panned programme (level differences) and as a spaced microphone pair (time differences), mixed into the
// film's own soundtrack (birds, water, wind) 15 dB below the burst's direct sound: every burst found, on the right side,
// its direction within 0.25.
// Skips (77) when the fixture is absent.
#include "media/audio/SoundLocator.hpp"
#include "media/core/linear_memory_arena.hpp"
#include "media/video/Perception.hpp"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace media;
namespace media::video { extern int g_trace; }
namespace {
int failures = 0;
const wchar_t* g_dump = nullptr; // --dump <dir>: frames 0, 100, 200 and 290 as raw 1280x720 luma (frame_NNN.raw).
bool g_verbose = false;          // --verbose: each picture's looming, each sound event.
int g_flows = -1;                // --flows N: the footage's tile flow at picture N (px/s).
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr double kPi = 3.14159265358979323846;
template <class T> void ReleaseCom(T*& p) { if (p) { p->Release(); p = nullptr; } }
std::uint32_t Hash(std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
double Noise(double u, double v, std::uint32_t seed) {
    const double fu = std::floor(u), fv = std::floor(v), a = u - fu, b = v - fv;
    const auto at = [&](int i, int j) { return static_cast<double>(Hash(static_cast<std::uint32_t>(i) * 7919u + static_cast<std::uint32_t>(j) * 104729u + seed) % 181u); };
    const int i = static_cast<int>(fu), j = static_cast<int>(fv);
    return (1 - a) * (1 - b) * at(i, j) + a * (1 - b) * at(i + 1, j) + (1 - a) * b * at(i, j + 1) + a * b * at(i + 1, j + 1);
}
// Media Foundation's source reader: pictures as NV12 (their luma), sound as 32-bit float.
struct Reader {
    IMFSourceReader* reader = nullptr;
    unsigned width = 0, height = 0, stride = 0, rate = 0, channels = 0;
    bool Open(const wchar_t* path, bool video) {
        IMFAttributes* attributes = nullptr;
        if (FAILED(MFCreateAttributes(&attributes, 1))) return false;
        (void)attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        const bool made = SUCCEEDED(MFCreateSourceReaderFromURL(path, attributes, &reader));
        ReleaseCom(attributes);
        if (!made) return false;
        const DWORD stream = video ? static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM) : static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        (void)reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        (void)reader->SetStreamSelection(stream, TRUE);
        IMFMediaType* type = nullptr;
        if (FAILED(MFCreateMediaType(&type))) return false;
        (void)type->SetGUID(MF_MT_MAJOR_TYPE, video ? MFMediaType_Video : MFMediaType_Audio);
        (void)type->SetGUID(MF_MT_SUBTYPE, video ? MFVideoFormat_NV12 : MFAudioFormat_Float);
        const bool set = SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, type));
        ReleaseCom(type);
        if (!set || FAILED(reader->GetCurrentMediaType(stream, &type))) return false;
        UINT32 w = 0, h = 0;
        if (video) { (void)MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h); width = w; height = h; stride = static_cast<unsigned>(MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, w)); }
        else { rate = MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0); channels = MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 0); }
        ReleaseCom(type);
        return video ? width && height : rate && channels;
    }
    // The next sample's bytes appended to `out`; its time. False at the end.
    bool Next(bool video, std::vector<std::uint8_t>& out, std::int64_t& pts) {
        for (;;) {
            DWORD flags = 0; LONGLONG time = 0; IMFSample* sample = nullptr;
            const DWORD stream = video ? static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM) : static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
            if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, &time, &sample)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { ReleaseCom(sample); return false; }
            if (!sample) continue;
            IMFMediaBuffer* buffer = nullptr; BYTE* data = nullptr; DWORD length = 0;
            const bool ok = SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length));
            if (ok) { out.insert(out.end(), data, data + length); buffer->Unlock(); }
            ReleaseCom(buffer); ReleaseCom(sample);
            pts = time;
            return ok;
        }
    }
    ~Reader() { ReleaseCom(reader); }
};
// The whole picture's translation from `before` to `now` (luma, w x h, stride w) at full resolution: SAD over +-r pixels on a
// 4x-subsampled central window, then refined at full resolution. `confidence`: the second-best cost over the best (>1.15:
// unambiguous).
void Register(const std::uint8_t* before, const std::uint8_t* now, unsigned w, unsigned h, int r, int& bx, int& by, double& confidence) {
    const auto cost = [&](int dx, int dy, unsigned step) {
        double sad = 0;
        for (unsigned y = h / 4; y < 3 * h / 4; y += step)
            for (unsigned x = w / 4; x < 3 * w / 4; x += step)
                sad += std::abs(static_cast<int>(now[y * w + x]) - static_cast<int>(before[(static_cast<int>(y) - dy) * static_cast<int>(w) + static_cast<int>(x) - dx]));
        return sad;
    };
    double best = 1e300, second = 1e300; bx = by = 0;
    for (int dy = -r; dy <= r; dy += 2)
        for (int dx = -r; dx <= r; dx += 2) {
            const double c = cost(dx, dy, 4);
            if (c < best) { second = best; best = c; bx = dx; by = dy; } else if (c < second && (std::abs(dx - bx) > 4 || std::abs(dy - by) > 4)) second = c;
        }
    const int cx = bx, cy = by; best = 1e300;
    for (int dy = cy - 2; dy <= cy + 2; ++dy) for (int dx = cx - 2; dx <= cx + 2; ++dx) { const double c = cost(dx, dy, 2); if (c < best) { best = c; bx = dx; by = dy; } }
    confidence = second / (std::max)(cost(cx, cy, 4), 1.0);
}
double Median(std::vector<double> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; }

int TestVideo(const wchar_t* path) {
    Reader video;
    if (!video.Open(path, true)) { std::puts("FAIL: the fixture's video could not be decoded"); ++failures; return 0; }
    const unsigned W = video.width, H = video.height, CW = 960, CH = 540;
    std::printf("Footage: %ux%u NV12 (stride %u)\n", W, H, video.stride);
    if (W != 1280 || H != 720) { std::puts("FAIL: expected 1280x720"); ++failures; return 0; }
    core::LinearMemoryArena arena; if (!arena.Initialize(16u << 20)) return 0;
    video::Perception full, still, shaken, loom, pitched[3];
    CHECK(full.Configure(arena, W, H, 0) && still.Configure(arena, CW, CH, 0) && shaken.Configure(arena, CW, CH, 0) && loom.Configure(arena, W, H, 0));
    for (auto& p : pitched) CHECK(p.Configure(arena, CW, CH, 0));
    std::vector<std::uint8_t> window(CW * CH); unsigned pitched_looming[3]{}, pitched_ground[3]{}, pitched_support[3]{}; double pitch_before = 0;
    // E: a 640x480 window panning across the footage, at every SIMD level this build and processor have.
    constexpr unsigned PW = 640, PH = 480;
    const unsigned top_level = video::Perception::ForceLevel(2);
    video::Perception panned[3];
    for (unsigned level = 0; level <= top_level; ++level) CHECK(panned[level].Configure(arena, PW, PH, 0));
    std::vector<std::uint8_t> pan(PW * PH); double pan_us[3]{}, pan_coverage = 0; unsigned pan_compared = 0, pan_differing = 0, pan_frames = 0;
    std::vector<std::uint8_t> bytes, previous(W * H), crop(CW * CH), crop_still(CW * CH), pasted(W * H);
    const double fps = 59.94, crop_focal = CW / 2.0; // 90 degrees across the window.
    std::vector<double> error_a, motion_diff, ttc_error; unsigned confident = 0, looming_a = 0, frames = 0, looming_b = 0, looming_still = 0;
    double coverage = 0, salient_a = 0, salient_b = 0, salient_still = 0, hazard_off = 0; unsigned looming_c = 0, c_frames = 0, early = 0, early_found = 0;
    unsigned ground_a = 0, fovea_c = 0, early_support = UINT_MAX; std::vector<double> early_error;
    double sx_before = 0, sy_before = 0;
    std::int64_t first_pts = -1;
    for (unsigned f = 0; f < 300; ++f) {
        bytes.clear(); std::int64_t pts = 0;
        if (!video.Next(true, bytes, pts)) break;
        if (bytes.size() < static_cast<std::size_t>(video.stride) * H) continue;
        if (first_pts < 0) first_pts = pts;
        std::vector<std::uint8_t> luma(W * H);
        for (unsigned y = 0; y < H; ++y) std::memcpy(luma.data() + y * W, bytes.data() + static_cast<std::size_t>(y) * video.stride, W);
        if (g_dump && (f == 0 || f == 100 || f == 200 || f == 290)) {
            wchar_t name[2048]; swprintf(name, 2048, L"%s\\frame_%03u.raw", g_dump, f);
            FILE* out = nullptr;
            if (_wfopen_s(&out, name, L"wb") == 0 && out) { std::fwrite(luma.data(), 1, luma.size(), out); std::fclose(out); }
        }
        // A: the footage.
        const auto& a = full.Push(luma.data(), W, pts, {});
        if (g_flows >= 0 && f == static_cast<unsigned>(g_flows)) {
            std::printf("Flow of picture %u (px/s; . unreliable):\n", f);
            for (unsigned ty = 0; ty < video::Perception::kTiles; ++ty) {
                for (unsigned tx = 0; tx < video::Perception::kTiles; ++tx) {
                    const unsigned t = ty * video::Perception::kTiles + tx;
                    if (full.FlowValid(t)) std::printf(" %4.0f,%4.0f", static_cast<double>(full.FlowX(t)), static_cast<double>(full.FlowY(t)));
                    else std::printf("     .     ");
                }
                std::printf("\n");
            }
        }
        if (g_verbose && a.looming) std::printf("  picture %u: looming, divergence %.2f/s, TTC %.2f s at (%.2f, %.2f); motion (%.3f, %.3f)\n", f, static_cast<double>(a.divergence),
                                                static_cast<double>(a.ttc_seconds), static_cast<double>(a.hazard_x), static_cast<double>(a.hazard_y),
                                                static_cast<double>(a.motion_x), static_cast<double>(a.motion_y));
        // B: the window, still and shaken (the window moving right by sx shows the scene moved left: the camera turned right).
        const double t = f / fps, sx = 12 * std::sin(2 * kPi * 7 * t), sy = 9 * std::sin(2 * kPi * 9 * t + 1);
        const int ox = 160 + static_cast<int>(std::lround(sx)), oy = 90 + static_cast<int>(std::lround(sy));
        for (unsigned y = 0; y < CH; ++y) {
            std::memcpy(crop.data() + y * CW, luma.data() + static_cast<std::size_t>(oy + static_cast<int>(y)) * W + ox, CW);
            std::memcpy(crop_still.data() + y * CW, luma.data() + static_cast<std::size_t>(90 + y) * W + 160, CW);
        }
        const double rx = std::lround(sx), ry = std::lround(sy);
        const video::EgoMotion imu{static_cast<float>((rx - sx_before) / crop_focal), static_cast<float>(-(ry - sy_before) / crop_focal), 0, f > 0};
        sx_before = rx; sy_before = ry;
        const auto& b = shaken.Push(crop.data(), CW, pts, imu);
        const auto& s = still.Push(crop_still.data(), CW, pts, {});
        // D: the window at the top (horizon low), at the bottom (horizon high), and pitching (its top 90 +- 80 px at 0.3 Hz).
        const int pitch_top = 90 + static_cast<int>(std::lround(80 * std::sin(2 * kPi * 0.3 * t)));
        const int tops[3]{0, 180, pitch_top};
        for (unsigned k = 0; k < 3; ++k) {
            for (unsigned y = 0; y < CH; ++y) std::memcpy(window.data() + y * CW, luma.data() + static_cast<std::size_t>(tops[k] + static_cast<int>(y)) * W + 160, CW);
            // The window moving down shows the scene moving up: the camera tilted down (negative pitch).
            const video::EgoMotion tilt{0, static_cast<float>(-(pitch_top - pitch_before) / crop_focal), 0, k == 2 && f > 0};
            const auto& d = pitched[k].Push(window.data(), CW, pts, k == 2 ? tilt : video::EgoMotion{});
            if (f >= 8) { pitched_looming[k] += d.looming; pitched_ground[k] += d.ground; pitched_support[k] = (std::max)(pitched_support[k], d.support); }
            if (g_verbose && f >= 8 && (d.looming || d.support)) std::printf("  D %s picture %u: looming %d, support %u windows, divergence %.2f at (%.2f, %.2f)%s; ground %d horizon %.2f (%u windows)\n",
                                                                     k == 0 ? "low" : k == 1 ? "high" : "pitching", f, d.looming ? 1 : 0, d.support, static_cast<double>(d.divergence),
                                                                     static_cast<double>(d.hazard_x), static_cast<double>(d.hazard_y), d.fovea ? " fovea" : "", d.ground ? 1 : 0,
                                                                     static_cast<double>(d.horizon_y), d.ground_windows);
        }
        pitch_before = pitch_top;
        // E: the window's left edge from 40 px, 2 px a picture to the right (120 px/s; the scene moves left: the camera turns
        // right, the IMU's yaw 2 / 320 a picture for 90 degrees across 640), rows 160 to 640 (clouds, trees, water, sand).
        {
            const unsigned left = 40 + 2 * f;
            for (unsigned y = 0; y < PH; ++y) std::memcpy(pan.data() + y * PW, luma.data() + static_cast<std::size_t>(160 + y) * W + left, PW);
            const video::EgoMotion yaw{f ? 2.0f / 320.0f : 0.0f, 0, 0, f > 0};
            video::PerceptionResult out[3];
            for (unsigned level = 0; level <= top_level; ++level) {
                (void)video::Perception::ForceLevel(level);
                const auto began = std::chrono::steady_clock::now();
                out[level] = panned[level].Push(pan.data(), PW, pts, yaw);
                pan_us[level] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
            }
            (void)video::Perception::ForceLevel(2);
            ++pan_frames; pan_coverage += out[0].coverage;
            for (unsigned level = 1; level <= top_level; ++level) {
                const auto& x = out[level]; const auto& r = out[0];
                bool same = x.looming == r.looming && x.divergence == r.divergence && x.ttc_seconds == r.ttc_seconds && x.hazard_x == r.hazard_x && x.hazard_y == r.hazard_y &&
                            x.motion_x == r.motion_x && x.motion_y == r.motion_y && x.coverage == r.coverage && x.salient == r.salient && x.fovea == r.fovea &&
                            x.ground == r.ground && x.ground_windows == r.ground_windows && x.support == r.support && x.horizon_y == r.horizon_y;
                for (unsigned tile = 0; tile < video::Perception::kTiles * video::Perception::kTiles; ++tile)
                    same = same && panned[level].FlowValid(tile) == panned[0].FlowValid(tile) && panned[level].FlowX(tile) == panned[0].FlowX(tile) &&
                           panned[level].FlowY(tile) == panned[0].FlowY(tile) && panned[level].Saliency(tile) == panned[0].Saliency(tile);
                ++pan_compared; pan_differing += same ? 0u : 1u;
            }
        }
        // C: an obstacle approaching at 1 m/s from 3 m (its half size 100 px at 3 m: as many tiles as the synthetic test's), centred at (0.55, 0.45).
        const double z = 3.0 - (pts - first_pts) / 1e7, half = 100 * 3.0 / z, cx = 0.55 * W, cy = 0.45 * H;
        pasted = luma;
        for (int y = static_cast<int>(cy - half); y < static_cast<int>(cy + half); ++y)
            for (int x = static_cast<int>(cx - half); x < static_cast<int>(cx + half); ++x)
                if (x >= 0 && y >= 0 && x < static_cast<int>(W) && y < static_cast<int>(H))
                    pasted[static_cast<std::size_t>(y) * W + x] = static_cast<std::uint8_t>(30 + Noise((x - cx) / half * 6 + 50, (y - cy) / half * 6 + 50, 11));
        const auto& c = loom.Push(pasted.data(), W, pts, {});
        if (f >= 8) {
            ++frames; coverage += a.coverage; salient_a += a.salient_fraction; looming_a += a.looming; ground_a += a.ground;
            looming_b += b.looming; looming_still += s.looming;
            salient_b += b.salient_fraction; salient_still += s.salient_fraction;
            motion_diff.push_back(std::hypot((b.motion_x - s.motion_x) * CW, (b.motion_y - s.motion_y) * CH));
            int dx = 0, dy = 0; double confidence = 0;
            Register(previous.data(), luma.data(), W, H, 24, dx, dy, confidence);
            if (confidence > 1.15 && a.coverage > 0.3f) {
                ++confident;
                // Pictures per second x pixels per picture, against the tiles' median (widths/s x width).
                const double vx = dx * fps, vy = dy * fps, mx = a.motion_x * W, my = a.motion_y * H;
                error_a.push_back(std::hypot(vx - mx, vy - my) / (std::max)(15.0, std::hypot(vx, vy)));
            }
            if (g_verbose && f >= 12 && z > 1.2) std::printf("  C picture %u: truth TTC %.2f s; looming %d%s, TTC %.2f s at (%.2f, %.2f), divergence %.2f, support %u; ground %d (%u windows); the footage's own: %d, %.2f s\n", f, z,
                                                       c.looming ? 1 : 0, c.fovea ? " (fovea)" : "", static_cast<double>(c.ttc_seconds), static_cast<double>(c.hazard_x), static_cast<double>(c.hazard_y),
                                                       static_cast<double>(c.divergence), c.support, c.ground ? 1 : 0, c.ground_windows, a.looming ? 1 : 0, static_cast<double>(a.ttc_seconds));
            const bool strongest = !a.looming || z < 0.8 * a.ttc_seconds;
            if (z > 2.0 && f >= 12 && strongest) {
                ++early;
                if (c.looming && std::hypot((c.hazard_x - 0.55) * W, (c.hazard_y - 0.45) * H) < half) {
                    ++early_found; fovea_c += c.fovea; early_error.push_back(std::fabs(c.ttc_seconds - z) / z);
                    early_support = (std::min)(early_support, c.support);
                }
            }
            if (z > 1.2 && z <= 2.0 && strongest) {
                ++c_frames; looming_c += c.looming;
                if (c.looming) {
                    ttc_error.push_back(std::fabs(c.ttc_seconds - z) / z);
                    hazard_off = (std::max)(hazard_off, std::hypot((c.hazard_x - 0.55) * W, (c.hazard_y - 0.45) * H) / half);
                }
            }
        }
        previous = luma;
    }
    const double n = (std::max)(frames, 1u);
    std::printf("A, the footage (%u pictures): flow covers %.0f %% of the tiles; dominant motion against the whole picture's registration (%u unambiguous): "
                "median error %.0f %%, 90th %.0f %%; looming on %u pictures (the ground the camera travels over found on %u); %.1f %% of the tiles salient on average\n",
                frames, 100 * coverage / n, confident, 100 * Median(error_a), [&] { auto v = error_a; if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return 100 * v[v.size() * 9 / 10]; }(),
                looming_a, ground_a, 100 * salient_a / n);
    std::printf("B, shaken (+-12 / 9 px at 7 / 9 Hz with the IMU) against the still window: dominant motion differs by %.1f px/s median, %.1f at most; "
                "looming %u (still: %u); salient %.1f %% (still: %.1f %%)\n", Median(motion_diff),
                motion_diff.empty() ? 0.0 : *std::max_element(motion_diff.begin(), motion_diff.end()), looming_b, looming_still, 100 * salient_b / n, 100 * salient_still / n);
    std::printf("C, an obstacle approaching over the footage, where it is the strongest approach: within 2 s of contact, looming on %u of %u pictures, time to contact "
                "median error %.1f %%, hazard at most %.2f half-sizes from its centre; from 2 to 2.8 s, found on %u of %u (%u by the fovea), time to contact median error %.1f %%\n",
                looming_c, c_frames, 100 * Median(ttc_error), hazard_off, early_found, early, fovea_c, 100 * Median(early_error));
    std::printf("   the obstacle's support: at least %u windows on a picture where it was found from 2 to 2.8 s\n", early_found ? early_support : 0u);
    std::printf("D, pitched and cropped windows: horizon low (camera up): ground on %u, looming on %u; horizon high (camera down): ground on %u, looming on %u; "
                "pitching +-80 px at 0.3 Hz with the IMU: ground on %u, looming on %u (of %u pictures); the most windows on one object %u / %u / %u\n", pitched_ground[0],
                pitched_looming[0], pitched_ground[1], pitched_looming[1], pitched_ground[2], pitched_looming[2], frames, pitched_support[0], pitched_support[1], pitched_support[2]);
    const char* names[]{"scalar", "SSE2", "AVX2"};
    std::printf("E, a 640x480 window panning across the footage (120 px/s with the IMU, %u pictures, flow on %.0f %% of the tiles): %u picture comparisons against the scalar "
                "loop, %u differing; us per picture:", pan_frames, 100 * pan_coverage / (std::max)(pan_frames, 1u), pan_compared, pan_differing);
    for (unsigned level = 0; level <= top_level; ++level) std::printf(" %s %.0f", names[level], pan_us[level] / (std::max)(pan_frames, 1u));
    std::printf("\n");
    CHECK(frames >= 200 && coverage / n > 0.25 && confident >= 50 && Median(error_a) < 0.25);
    // Measured 2026-10-29 (deterministic): ground on 292 / 272 / 188, looming on 0 / 0 / 0 (2026-10-28: ground on 45 / 200 / 135,
    // looming on 10 / 31 / 13; the 2026-10-27 bottom-rows model: ground on 0 / 200 / 121, looming on 10 / 55 / 21).
    CHECK(pitched_ground[0] >= 200 && pitched_looming[0] <= 8);
    CHECK(pitched_ground[1] >= frames / 2 && pitched_looming[1] <= 8);
    CHECK(pitched_ground[2] >= 150 && pitched_looming[2] <= 8);
    CHECK(looming_a <= 8 && ground_a >= frames / 2);
    CHECK(Median(motion_diff) < 12 && looming_b <= looming_still + 2 && salient_b < salient_still + 0.02 * n);
    CHECK(c_frames >= 10 && looming_c >= 0.8 * c_frames && Median(ttc_error) < 0.15 && hazard_off < 1.0);
    // The directive's 45 of 46 (2026-10-29): found on 47 of 48.
    CHECK(early >= 10 && early_found * 46 >= early * 45 && Median(early_error) < 0.15);
    CHECK(pan_frames >= 200 && pan_differing == 0 && pan_compared == pan_frames * top_level);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(pan_us[top_level] / pan_frames <= 600); // The directive's 0.6 ms (optimised builds).
#endif
    return 1;
}
// ---- Audio ----
struct Room {
    // The impulse response from a source to one ear: the direct sound (gain g, delay d samples), six early reflections and an
    // exponentially decaying diffuse tail (RT60), its noise decorrelated per ear (seed).
    std::vector<float> ir;
    Room(unsigned rate, double gain, double delay, double rt60, std::uint32_t seed, double direct_to_reverb_db) {
        ir.assign(static_cast<std::size_t>(rate * rt60), 0.0f);
        const auto put = [&](double at, double g) { const auto i = static_cast<std::size_t>(at); if (i + 1 < ir.size()) { const double f = at - i; ir[i] += static_cast<float>(g * (1 - f)); ir[i + 1] += static_cast<float>(g * f); } };
        put(delay, gain);
        double tail = 0;
        for (unsigned k = 0; k < 6; ++k) put(delay + rate * (0.004 + 0.003 * k + 0.0007 * (Hash(seed + k) % 5)), gain * 0.5 * std::pow(0.8, k) * ((Hash(seed * 3 + k) & 1) ? 1 : -1));
        const double decay = std::log(1000.0) / (rt60 * rate);
        std::vector<double> t(ir.size(), 0);
        for (std::size_t i = static_cast<std::size_t>(delay + 0.02 * rate); i < ir.size(); ++i) {
            const double v = ((Hash(seed * 7919u + static_cast<std::uint32_t>(i)) % 20001) / 10000.0 - 1) * std::exp(-decay * (i - delay));
            t[i] = v; tail += v * v;
        }
        const double want = gain * gain * std::pow(10, -direct_to_reverb_db / 10); // Tail energy against the direct sound's.
        const double k = tail > 0 ? std::sqrt(want / tail) : 0;
        for (std::size_t i = 0; i < ir.size(); ++i) ir[i] += static_cast<float>(t[i] * k);
    }
};
int TestAudio(const wchar_t* path) {
    Reader sound;
    if (!sound.Open(path, false)) { std::puts("FAIL: the fixture's audio could not be decoded"); ++failures; return 0; }
    const unsigned rate = sound.rate, channels = sound.channels;
    std::vector<std::uint8_t> bytes; std::int64_t pts = 0;
    while (bytes.size() < static_cast<std::size_t>(rate) * channels * 4 * 12 && sound.Next(false, bytes, pts)) {}
    const std::size_t frames = bytes.size() / 4 / channels;
    std::vector<float> bed(frames * 2);
    const auto* f = reinterpret_cast<const float*>(bytes.data());
    double bed_energy = 0;
    for (std::size_t i = 0; i < frames; ++i) { bed[2 * i] = f[i * channels]; bed[2 * i + 1] = f[i * channels + (channels > 1 ? 1 : 0)]; bed_energy += bed[2 * i] * bed[2 * i] + bed[2 * i + 1] * bed[2 * i + 1]; }
    const double bed_rms = std::sqrt(bed_energy / (2.0 * frames));
    std::printf("Soundtrack: %u Hz, %u channels, %.1f s, %.1f dBFS RMS\n", rate, channels, static_cast<double>(frames) / rate, 20 * std::log10(bed_rms + 1e-12));
    CHECK(frames > static_cast<std::size_t>(rate) * 6 && bed_rms > 1e-4);
    const double azimuths[]{-0.8, -0.4, 0.4, 0.8};
    for (unsigned mode = 0; mode < 2; ++mode) { // 0: a panned programme (levels); 1: a spaced pair 0.2 m apart (times).
        std::vector<float> mix(bed);
        // The bursts' direct sound 15 dB above the soundtrack's level; the bed as it is.
        const double burst_rms = bed_rms * std::pow(10, 15 / 20.0);
        const unsigned burst = rate * 30 / 1000;
        for (unsigned k = 0; k < 8; ++k) {
            const double a = azimuths[k % 4] * (k < 4 ? 1 : -1); // Each direction twice, in two orders.
            double gl = 1, gr = 1, dl = 0, dr = 0;
            if (mode == 0) { const double angle = (a + 1) * kPi / 4; gl = std::cos(angle) * std::sqrt(2.0); gr = std::sin(angle) * std::sqrt(2.0); }
            else { const double itd = a * 0.2 / 343 * rate; dl = itd > 0 ? itd : 0; dr = itd < 0 ? -itd : 0; } // The far side hears it later.
            const Room left(rate, gl, 2 + dl, 0.5, 100 + k, 3), right(rate, gr, 2 + dr, 0.5, 200 + k, 3);
            const auto at = static_cast<std::size_t>(std::llround((1.1 + 1.3 * k) * rate)); // 1.1 s, then every 1.3 s.
            for (unsigned i = 0; i < burst; ++i) {
                const double s = burst_rms * ((Hash(9000u + k * 7777u + i) % 20001) / 10000.0 - 1) * std::sqrt(3.0);
                for (std::size_t j = 0; j < left.ir.size() && at + i + j < frames; ++j) {
                    mix[2 * (at + i + j)] += static_cast<float>(s * left.ir[j]); mix[2 * (at + i + j) + 1] += static_cast<float>(s * right.ir[j]);
                }
            }
        }
        audio::SoundLocator locator;
        CHECK(locator.Configure(rate, 2, 0.2f));
        unsigned found = 0, right_side = 0; double worst = 0; std::uint32_t seen = 0;
        double azimuth_at[8]{}; bool heard[8]{};
        for (std::size_t i = 0; i < frames; i += rate / 100) {
            const auto count = static_cast<std::uint32_t>((std::min)(static_cast<std::size_t>(rate / 100), frames - i));
            locator.Process(mix.data() + 2 * i, count, static_cast<std::int64_t>(i) * 10000000 / rate);
            audio::SoundEvent e;
            if (locator.Latest(e) && e.sequence != seen) {
                seen = e.sequence;
                if (g_verbose) std::printf("  event at %.3f s: azimuth %+.2f, level %.1f dB, ILD %+.1f dB, ITD %+.0f us, correlation %.2f\n", e.pts_100ns / 1e7,
                                           static_cast<double>(e.azimuth), static_cast<double>(e.level_db), static_cast<double>(e.ild_db), static_cast<double>(e.itd_us),
                                           static_cast<double>(e.correlation));
                // Which burst: the one whose start is within 60 ms before the onset.
                for (unsigned k = 0; k < 8; ++k) {
                    const double start = 1.1 + k * 1.3, onset = e.pts_100ns / 1e7;
                    if (onset >= start - 0.02 && onset < start + 0.06 && !heard[k]) { heard[k] = true; azimuth_at[k] = e.azimuth; }
                }
            }
        }
        for (unsigned k = 0; k < 8; ++k) {
            const double a = azimuths[k % 4] * (k < 4 ? 1 : -1);
            if (!heard[k]) continue;
            ++found; if ((azimuth_at[k] > 0) == (a > 0)) ++right_side;
            worst = (std::max)(worst, std::fabs(azimuth_at[k] - a));
        }
        std::printf("%s, RT60 0.5 s, 15 dB over the soundtrack: %u of 8 bursts found, %u on the right side, worst direction error %.2f (", mode ? "Spaced pair (times)" : "Panned (levels)",
                    found, right_side, worst);
        for (unsigned k = 0; k < 8; ++k) std::printf("%s%+.1f->%+.2f", k ? " " : "", azimuths[k % 4] * (k < 4 ? 1 : -1), heard[k] ? azimuth_at[k] : 0.0);
        std::printf("); %u events in all\n", locator.Events());
        CHECK(found == 8 && right_side == 8 && worst < 0.25 && locator.Events() <= 10);
    }
    return 1;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    for (int i = 2; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--dump") == 0 && i + 1 < argc) g_dump = argv[++i];
        else if (std::wcscmp(argv[i], L"--verbose") == 0) g_verbose = true;
        else if (std::wcscmp(argv[i], L"--flows") == 0 && i + 1 < argc) g_flows = _wtoi(argv[++i]);
    }
    wchar_t path[2048];
    swprintf(path, 2048, L"%s\\costa_rica_4k60_hdr.mp4", argv[1]);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { std::puts("SKIP fixture not present"); return 77; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION))) { std::puts("FAIL: Media Foundation"); return 1; }
    TestVideo(path);
    TestAudio(path);
    MFShutdown(); CoUninitialize();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "real perception tests passed", failures);
    return failures ? 1 : 0;
}
