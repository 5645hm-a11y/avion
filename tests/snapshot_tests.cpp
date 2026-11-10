// Forensic snapshots (docs/picture.md, "Forensic snapshots"; docs/sdk.md, "Snapshots", API 8), 2026-11-01. Suite #200.
// Portable (MSVC; GCC -O2 and ASan/UBSan):
// - the encoder: an odd-sized RGBA image (37 x 23: rows padded) to a BMP in memory: the header as written (BM, 54-byte offset,
//   24 bits, BI_RGB, a negative height: top-down), every pixel back as it was, the padding zero, the frame hash equal to an
//   FNV-1a 64 computed here and to SnapshotEncoder::HashRgba; no allocation;
// - the sidecar: well-formed JSON (a parser here) with the fields as given (the hash, the time, the view, the orientation, the
//   objects with an occluded one and the target), a path's quotes and backslashes escaped, UTF-8 kept; too small a buffer
//   writes nothing; the sidecar's path and a snapshot's name;
// - the commands: "take snapshot", "save frame", "צילום מסך", "capturer l'image" and the other languages parse to Snapshot,
//   while "pin frame", "save this point" and "שמור פריים להשוואה" keep theirs;
// - the SDK in host mode with a camera (no GPU path): after "zoom in" and a quarter turn, media_engine_save_snapshot writes the
//   view (its 320 x 240 region turned: 240 x 320) whose every pixel is the picture's sample converted here (BT.709, limited
//   range) within one code; the file's own hash is media_query_snapshot's and the sidecar's; the sidecar says "view", the
//   zoom, the region, the turn, and lists the objects telemetry has (the same ids); "take snapshot" writes a second one to a
//   free name in the snapshot folder; no allocation while saving.
// Windows, with a hardware Direct3D 12 device (MEDIA_SENSOR_GPU): with shadow lift, a 2x zoom and a 3-degree leveling turn the
//   snapshot is the GPU image exactly (every byte of R G B; the same hash), the sidecar saying so ("gpu-image", the lift, the
//   level). And the windowed engine through the SDK on the two-targets fixture (hidden): "zoom in", then "take snapshot" (a
//   second one at once is refused: one is on its way), then media_engine_save_snapshot to a path: both written by the snapshot
//   thread while the pictures keep coming (0 dropped), each file's hash its record's, the sidecar "window" with the zoom and
//   the object table; no allocation in the commands.
#include "media/c_api.h"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/video/Snapshot.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "shell32.lib")
#endif
#endif

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr unsigned kW = 640, kH = 480;

std::uint64_t Fnv(const std::uint8_t* p, std::size_t n, std::uint64_t h = 14695981039346656037ull) {
    for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
std::uint32_t Get32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint16_t Get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
// A file's bytes (UTF-8 path; wide on Windows).
bool ReadAll(const char* path, std::vector<std::uint8_t>& out) {
    out.clear();
#if defined(_WIN32)
    wchar_t wide[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024) <= 0) return false;
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, wide, L"rb") != 0) f = nullptr;
#else
    std::FILE* f = std::fopen(path, "rb");
#endif
    if (!f) return false;
    std::uint8_t chunk[65536];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) out.insert(out.end(), chunk, chunk + n);
    std::fclose(f);
    return true;
}
// MEDIA_KEEP_SNAPSHOTS set: the files are left where they were written (to look at them).
bool Keep() {
#if defined(_WIN32)
    return GetEnvironmentVariableA("MEDIA_KEEP_SNAPSHOTS", nullptr, 0) > 0;
#else
    return std::getenv("MEDIA_KEEP_SNAPSHOTS") != nullptr;
#endif
}
void Remove(const char* path) {
    if (Keep()) { std::printf("  kept %s\n", path); return; }
#if defined(_WIN32)
    wchar_t wide[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024) > 0) (void)DeleteFileW(wide);
#else
    (void)std::remove(path);
#endif
}
// A BMP as the encoder writes it: its size and its pixels as R G B rows top to bottom; false when the header is not that.
struct Bmp { unsigned width = 0, height = 0; std::vector<std::uint8_t> rgb; };
bool ParseBmp(const std::vector<std::uint8_t>& f, Bmp& out) {
    if (f.size() < 54 || f[0] != 'B' || f[1] != 'M' || Get32(&f[2]) != f.size() || Get32(&f[10]) != 54 || Get32(&f[14]) != 40) return false;
    const auto width = static_cast<std::int32_t>(Get32(&f[18])), height = static_cast<std::int32_t>(Get32(&f[22]));
    if (width <= 0 || height >= 0 || Get16(&f[26]) != 1 || Get16(&f[28]) != 24 || Get32(&f[30]) != 0) return false;
    out.width = static_cast<unsigned>(width); out.height = static_cast<unsigned>(-height);
    const std::size_t row = (3 * out.width + 3) & ~3u;
    if (Get32(&f[34]) != row * out.height || f.size() != 54 + row * out.height) return false;
    out.rgb.resize(std::size_t{3} * out.width * out.height);
    for (unsigned y = 0; y < out.height; ++y) {
        const std::uint8_t* in = &f[54 + row * y];
        for (unsigned x = 0; x < out.width; ++x) for (unsigned k = 0; k < 3; ++k) out.rgb[(std::size_t{y} * out.width + x) * 3 + k] = in[3 * x + 2 - k];
        for (std::size_t i = 3 * out.width; i < row; ++i) if (in[i]) return false; // The padding is zero.
    }
    return true;
}
// A small JSON parser: whether `text` is one well-formed value (objects, arrays, strings with escapes, numbers, literals).
struct Json {
    const char* p; const char* end;
    void Space() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool Literal(const char* word) { const auto n = std::strlen(word); if (static_cast<std::size_t>(end - p) < n || std::strncmp(p, word, n)) return false; p += n; return true; }
    bool String() {
        if (p >= end || *p != '"') return false;
        for (++p; p < end && *p != '"'; ++p) {
            if (static_cast<unsigned char>(*p) < 0x20) return false;
            if (*p == '\\') { ++p; if (p >= end || !std::strchr("\"\\/bfnrtu", *p)) return false; if (*p == 'u') { for (int i = 0; i < 4; ++i) if (++p >= end || !std::isxdigit(static_cast<unsigned char>(*p))) return false; } }
        }
        if (p >= end) return false;
        ++p; return true;
    }
    bool Number() {
        const char* start = p;
        if (p < end && *p == '-') ++p;
        while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
        if (p < end && *p == '.') { ++p; while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p; }
        return p > start && std::isdigit(static_cast<unsigned char>(p[-1]));
    }
    bool Value() {
        Space();
        if (p >= end) return false;
        bool ok = false;
        if (*p == '{') {
            ++p; Space();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) { Space(); if (!String()) return false; Space(); if (p >= end || *p++ != ':') return false; if (!Value()) return false; Space();
                       if (p < end && *p == ',') { ++p; continue; } if (p < end && *p == '}') { ++p; return true; } return false; }
        } else if (*p == '[') {
            ++p; Space();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) { if (!Value()) return false; Space(); if (p < end && *p == ',') { ++p; continue; } if (p < end && *p == ']') { ++p; return true; } return false; }
        } else if (*p == '"') ok = String();
        else if (*p == 't') ok = Literal("true"); else if (*p == 'f') ok = Literal("false"); else if (*p == 'n') ok = Literal("null");
        else ok = Number();
        return ok;
    }
    static bool Valid(const std::string& text) { Json j{text.data(), text.data() + text.size()}; if (!j.Value()) return false; j.Space(); return j.p == j.end; }
};
bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
// The track ids a sidecar lists, in order.
std::vector<unsigned> SidecarIds(const std::string& text) {
    std::vector<unsigned> ids;
    for (std::size_t at = 0; (at = text.find("\"track_id\": ", at)) != std::string::npos; at += 12) ids.push_back(static_cast<unsigned>(std::strtoul(text.c_str() + at + 12, nullptr, 10)));
    return ids;
}
std::string HashText(std::uint64_t h) { char text[32]; std::snprintf(text, sizeof(text), "fnv1a64:%016llx", static_cast<unsigned long long>(h)); return text; }
std::string SidecarOf(const char* bmp) { std::string s = bmp; if (s.size() > 4) s.resize(s.size() - 4); return s + ".json"; }

// ---- The encoder ----
struct Memory { std::uint8_t bytes[8192]; std::size_t n = 0; };
bool ToMemory(void* context, const void* bytes, std::size_t count) noexcept {
    auto& m = *static_cast<Memory*>(context);
    if (count > sizeof(m.bytes) - m.n) return false;
    std::memcpy(m.bytes + m.n, bytes, count); m.n += count;
    return true;
}
video::SnapshotEncoder g_encoder; // 88 KiB: not on the stack.
void TestEncoder() {
    constexpr unsigned w = 37, h = 23, stride = 4 * w + 12;
    static std::uint8_t rgba[stride * h];
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        auto* p = rgba + y * stride + 4 * x;
        p[0] = static_cast<std::uint8_t>(x * 7 + y); p[1] = static_cast<std::uint8_t>(y * 11 + 3); p[2] = static_cast<std::uint8_t>((x ^ y) * 5); p[3] = 77;
    }
    static Memory memory; memory.n = 0;
    const video::RgbaImage image{rgba, stride};
    std::uint64_t hash = 0;
    allocation_probe::Begin();
    const bool wrote = g_encoder.WriteBmp(w, h, &video::SnapshotEncoder::RgbaRows, &image, {&memory, &ToMemory}, hash);
    const auto allocations = allocation_probe::End();
    std::vector<std::uint8_t> file(memory.bytes, memory.bytes + memory.n);
    Bmp bmp; const bool parsed = wrote && ParseBmp(file, bmp);
    std::vector<std::uint8_t> rgb;
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) for (unsigned k = 0; k < 3; ++k) rgb.push_back(rgba[y * stride + 4 * x + k]);
    const bool same = parsed && bmp.width == w && bmp.height == h && bmp.rgb == rgb;
    const auto expected = Fnv(rgb.data(), rgb.size());
    std::printf("Encoder: %ux%u RGBA -> %zu bytes of BMP (rows of %u bytes, top-down): header %s, pixels %s; hash %016llx (here %016llx, HashRgba %016llx); %llu allocations\n",
                w, h, memory.n, (3 * w + 3) & ~3u, parsed ? "right" : "WRONG", same ? "the same" : "DIFFERENT", static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(expected), static_cast<unsigned long long>(video::SnapshotEncoder::HashRgba(rgba, stride, w, h)),
                static_cast<unsigned long long>(allocations));
    CHECK(wrote && parsed && same && hash == expected && video::SnapshotEncoder::HashRgba(rgba, stride, w, h) == expected && allocations == 0);
    CHECK(memory.n == 54 + ((3 * w + 3) & ~3u) * h);
    // Out of range: nothing written.
    memory.n = 0;
    CHECK(!g_encoder.WriteBmp(0, h, &video::SnapshotEncoder::RgbaRows, &image, {&memory, &ToMemory}, hash) && memory.n == 0);
    CHECK(!g_encoder.WriteBmp(video::SnapshotEncoder::kMaxSide + 1, 1, &video::SnapshotEncoder::RgbaRows, &image, {&memory, &ToMemory}, hash));
}

// ---- The sidecar ----
void TestSidecar() {
    video::SnapshotInfo info;
    info.source = "gpu-image"; info.media = "C:\\clips\\\"night\" \xd7\xa6\xd7\x99\xd7\x9c\xd7\x95\xd7\x9d.mp4"; info.pts_100ns = 12345678; info.sequence = 3;
    info.view_zoom = 2; info.view_x = 0.25f; info.view_y = 0.75f; info.view_roll_degrees = -1.5f;
    info.region[0] = 10; info.region[1] = 20; info.region[2] = 330; info.region[3] = 260;
    info.quarter_turns = 1; info.mirrored = true; info.level_degrees = 3; info.auto_level = false; info.fitted = true;
    info.horizon_known = true; info.horizon_degrees = -2.25f; info.shadow_lift = 0.5f; info.reconstruction = "edge"; info.comparison = "original-enhanced";
    info.target_id = 7; info.object_count = 2;
    info.objects[0].track_id = 7; info.objects[0].x = 0.5f; info.objects[0].approaching = true; info.objects[0].ttc_seconds = 1.25f; info.objects[0].tiles = 9; info.objects[0].age = 30;
    info.objects[1].track_id = 9; info.objects[1].occluded = true; info.objects[1].vx = -0.31f;
    static char text[8192];
    allocation_probe::Begin();
    const auto n = video::FormatSidecar(info, "night_0h00m01.234s.bmp", 640, 480, 0x0123456789abcdefull, text, sizeof(text));
    const auto allocations = allocation_probe::End();
    const std::string s(text, n);
    const bool valid = n && Json::Valid(s);
    const char* parts[]{"\"format\": \"media-engine-snapshot/1\"", "\"image\": \"night_0h00m01.234s.bmp\"", "\"width\": 640", "\"height\": 480",
                        "\"frame_hash\": \"fnv1a64:0123456789abcdef\"", "\"source\": \"gpu-image\"", "\"pts_100ns\": 12345678", "\"time\": \"0:00:01.234\"",
                        "\"sequence\": 3", "\"zoom\": 2.000", "\"x\": 0.2500", "\"roll_degrees\": -1.50", "\"region\": [10, 20, 330, 260]", "\"quarter_turns\": 1",
                        "\"mirrored\": true", "\"level_degrees\": 3.0", "\"fitted\": true", "\"tilt_degrees\": -2.25", "\"shadow_lift\": 0.500",
                        "\"reconstruction\": \"edge\"", "\"comparison\": \"original-enhanced\"", "\"target_track_id\": 7", "\"ttc_seconds\": 1.250",
                        "\"occluded\": true", "\"target\": true", "\"velocity_x\": -0.3100",
                        "\"media\": \"C:\\\\clips\\\\\\\"night\\\" \xd7\xa6\xd7\x99\xd7\x9c\xd7\x95\xd7\x9d.mp4\""};
    unsigned found = 0;
    for (const char* part : parts) { if (Has(s, part)) ++found; else std::printf("  sidecar lacks %s\n", part); }
    const auto ids = SidecarIds(s);
    std::printf("Sidecar: %zu bytes, JSON %s, %u of %zu fields as given, objects %zu; %llu allocations\n", s.size(), valid ? "well-formed" : "MALFORMED", found,
                sizeof(parts) / sizeof(parts[0]), ids.size(), static_cast<unsigned long long>(allocations));
    CHECK(valid && found == sizeof(parts) / sizeof(parts[0]) && ids == std::vector<unsigned>({7, 9}) && allocations == 0);
    char small[64];
    CHECK(video::FormatSidecar(info, "x.bmp", 1, 1, 0, small, sizeof(small)) == 0);
    char path[64];
    CHECK(video::SidecarPath("a/b.BMP", path, sizeof(path)) && std::strcmp(path, "a/b.json") == 0);
    CHECK(video::SidecarPath("shot", path, sizeof(path)) && std::strcmp(path, "shot.json") == 0);
    CHECK(video::SnapshotName("my:clip", 36612345000, 1, path, sizeof(path)) && std::strcmp(path, "my_clip_1h01m01.234s.bmp") == 0);
    CHECK(video::SnapshotName(nullptr, 0, 2, path, sizeof(path)) && std::strcmp(path, "snapshot_0h00m00.000s_2.bmp") == 0);
    std::uint32_t region[4];
    video::ViewRegion(640, 480, 2, 0.5f, 0.5f, region);
    CHECK(region[0] == 160 && region[1] == 120 && region[2] == 480 && region[3] == 360);
    video::ViewRegion(640, 480, 4, 0.0f, 1.0f, region);
    CHECK(region[0] == 0 && region[1] == 360 && region[2] == 160 && region[3] == 480);
}

// ---- The commands ----
void TestCommands() {
    ai::MediaBounds film; film.duration_100ns = 600000000; film.seekable = true;
    const char* phrases[]{"take snapshot", "take a snapshot", "save frame", "screenshot", "capture this frame",
                          "\xd7\xa6\xd7\x99\xd7\x9c\xd7\x95\xd7\x9d \xd7\x9e\xd7\xa1\xd7\x9a",                                    // צילום מסך
                          "\xd7\xaa\xd7\xa6\xd7\x9c\xd7\x9d \xd7\xaa\xd7\x9e\xd7\x95\xd7\xa0\xd7\x94",                            // תצלם תמונה
                          "capturer l'image", "capture d'\xc3\xa9" "cran", "captura de pantalla", "guardar imagen", "Bildschirmfoto", "Bild speichern",
                          "\xd1\x81\xd0\xbd\xd0\xb8\xd0\xbc\xd0\xbe\xd0\xba \xd1\x8d\xd0\xba\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb0",    // снимок экрана
                          "\xd1\x81\xd0\xb4\xd0\xb5\xd0\xbb\xd0\xb0\xd0\xb9 \xd1\x81\xd0\xbd\xd0\xb8\xd0\xbc\xd0\xbe\xd0\xba",    // сделай снимок
                          "\xd9\x84\xd9\x82\xd8\xb7\xd8\xa9 \xd8\xb4\xd8\xa7\xd8\xb4\xd8\xa9",                                    // لقطة شاشة
                          "\xe6\x88\xaa\xe5\x9b\xbe",                                                                              // 截图
                          "\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\xbc\xe3\x83\xb3\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\x83\xe3\x83\x88"}; // スクリーンショット
    unsigned right = 0;
    for (const char* text : phrases) {
        const auto i = ai::LocalIntentDispatcher::Parse(text, film);
        if (i.status == ai::IntentStatus::Ok && i.kind == ai::IntentKind::Snapshot) ++right; else std::printf("  \"%s\" -> %s %s\n", text, ai::Name(i.kind), ai::Name(i.status));
    }
    const bool neighbours = ai::LocalIntentDispatcher::Parse("pin frame", film).kind == ai::IntentKind::PinFrame &&
                            ai::LocalIntentDispatcher::Parse("save this point", film).kind == ai::IntentKind::BookmarkAdd &&
                            ai::LocalIntentDispatcher::Parse("\xd7\xa9\xd7\x9e\xd7\x95\xd7\xa8 \xd7\xa4\xd7\xa8\xd7\x99\xd7\x99\xd7\x9d \xd7\x9c\xd7\x94\xd7\xa9\xd7\x95\xd7\x95\xd7\x90\xd7\x94", film).kind == ai::IntentKind::PinFrame;
    std::printf("Commands: %u of %zu parse to Snapshot; \"pin frame\", \"save this point\", \"\xd7\xa9\xd7\x9e\xd7\x95\xd7\xa8 \xd7\xa4\xd7\xa8\xd7\x99\xd7\x99\xd7\x9d \xd7\x9c\xd7\x94\xd7\xa9\xd7\x95\xd7\x95\xd7\x90\xd7\x94\" keep theirs: %s\n",
                right, sizeof(phrases) / sizeof(phrases[0]), neighbours ? "yes" : "NO");
    CHECK(right == sizeof(phrases) / sizeof(phrases[0]) && neighbours);
}

// ---- The SDK ----
constexpr std::size_t kStorage = 48u << 20;
std::vector<unsigned char> g_storage;
void* Aligned(unsigned char* base) {
    const std::size_t a = media_engine_storage_alignment();
    return base + (a - reinterpret_cast<std::uintptr_t>(base) % a) % a;
}
std::uint32_t Hash(std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
// The camera: a textured still background, a textured bright square moving right (an object for perception); chroma in four
// quadrants of their own colour.
std::uint8_t g_y[kW * kH], g_cb[kW * kH / 4], g_cr[kW * kH / 4], g_uv[kW * kH / 2];
void Draw(unsigned f) {
    const int sx = 120 + 6 * static_cast<int>(f), sy = 200;
    for (unsigned y = 0; y < kH; ++y)
        for (unsigned x = 0; x < kW; ++x) {
            int v = 40 + static_cast<int>(Hash(x / 6 * 977 + y / 6 * 7919) % 150);
            const int dx = static_cast<int>(x) - sx, dy = static_cast<int>(y) - sy;
            if (dx >= 0 && dx < 48 && dy >= 0 && dy < 48) v = 160 + static_cast<int>(Hash(static_cast<unsigned>(dx / 6 * 31 + dy / 6 * 131 + 5)) % 80);
            g_y[y * kW + x] = static_cast<std::uint8_t>(v);
        }
    for (unsigned y = 0; y < kH / 2; ++y)
        for (unsigned x = 0; x < kW / 2; ++x) {
            const bool right = x >= kW / 4, bottom = y >= kH / 4;
            const std::uint8_t cb = right ? (bottom ? 150 : 160) : (bottom ? 110 : 90), cr = right ? (bottom ? 150 : 90) : (bottom ? 110 : 160);
            g_cb[y * (kW / 2) + x] = cb; g_cr[y * (kW / 2) + x] = cr;
            g_uv[y * kW + 2 * x] = cb; g_uv[y * kW + 2 * x + 1] = cr;
        }
}
// The stored picture's sample (x, y) as R G B (BT.709, limited range), rounded.
void Expected(unsigned x, unsigned y, int rgb[3]) {
    const double l = 1.164384 * (g_y[y * kW + x] - 16.0), b = g_cb[(y / 2) * (kW / 2) + x / 2] - 128.0, r = g_cr[(y / 2) * (kW / 2) + x / 2] - 128.0;
    const double c[3]{l + 1.792741 * r, l - 0.213249 * b - 0.532909 * r, l + 2.112402 * b};
    for (int k = 0; k < 3; ++k) rgb[k] = static_cast<int>(std::lround(std::clamp(c[k], 0.0, 255.0)));
}
void TestHostView() {
    g_storage.assign(kStorage, 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = MEDIA_MODULE_TELEMETRY;
    media_engine_t* e = nullptr;
    CHECK(media_engine_create(&config, Aligned(g_storage.data()), kStorage - 4096, &e) == MEDIA_OK);
    if (!e) return;
    media_sensor_config_t sensor; media_sensor_config_init(&sensor);
    sensor.width = kW; sensor.height = kH;
    CHECK(media_engine_open_sensor(e, &sensor) == MEDIA_OK);
    media_snapshot_t none{}; none.struct_size = sizeof(none);
    CHECK(media_query_snapshot(e, &none) == MEDIA_ERROR_EMPTY);
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = kW; frame.height = kH;
    frame.planes[0] = g_y; frame.planes[1] = g_uv; frame.strides[0] = frame.strides[1] = kW;
    media_frame_telemetry_t t{}; t.struct_size = sizeof(t);
    for (unsigned f = 0; f < 24; ++f) {
        Draw(f); frame.timestamp_100ns = static_cast<std::int64_t>(f) * 333333;
        CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
        if (f == 10) CHECK(media_engine_command(e, "zoom in") == MEDIA_OK);
        if (f == 12) CHECK(media_engine_set_orientation(e, 1, 0) == MEDIA_OK);
    }
    (void)media_query_frame_telemetry(e, &t);
    const char* path = "snapshot_tests_view.bmp";
    allocation_probe::Begin();
    const auto saved = media_engine_save_snapshot(e, path);
    const auto allocations = allocation_probe::End();
    media_snapshot_t s{}; s.struct_size = sizeof(s);
    CHECK(saved == MEDIA_OK && media_query_snapshot(e, &s) == MEDIA_OK);
    std::vector<std::uint8_t> file; Bmp bmp;
    const bool parsed = ReadAll(path, file) && ParseBmp(file, bmp);
    // The view: zoom 2 at the centre, the region (160, 120) .. (480, 360), turned a quarter clockwise: out(ox, oy) is
    // stored(160 + oy, 359 - ox).
    int worst = 0; unsigned off = 0;
    if (parsed && bmp.width == 240 && bmp.height == 320)
        for (unsigned oy = 0; oy < 320; ++oy)
            for (unsigned ox = 0; ox < 240; ++ox) {
                int rgb[3]; Expected(160 + oy, 359 - ox, rgb);
                for (int k = 0; k < 3; ++k) {
                    const int d = std::abs(rgb[k] - bmp.rgb[(std::size_t{oy} * 240 + ox) * 3 + k]);
                    worst = (std::max)(worst, d); if (d > 1) ++off;
                }
            }
    const auto hash = parsed ? Fnv(bmp.rgb.data(), bmp.rgb.size()) : 0;
    std::vector<std::uint8_t> sidecar_bytes; const bool sidecar_read = ReadAll(SidecarOf(path).c_str(), sidecar_bytes);
    const std::string sidecar(sidecar_bytes.begin(), sidecar_bytes.end());
    std::vector<unsigned> telemetry_ids;
    for (unsigned i = 0; i < t.object_count; ++i) telemetry_ids.push_back(t.objects[i].track_id);
    const auto ids = SidecarIds(sidecar);
    const bool fields = sidecar_read && Json::Valid(sidecar) && Has(sidecar, "\"source\": \"view\"") && Has(sidecar, "\"zoom\": 2.000") &&
                        Has(sidecar, "\"region\": [160, 120, 480, 360]") && Has(sidecar, "\"quarter_turns\": 1") && Has(sidecar, HashText(hash).c_str()) &&
                        Has(sidecar, "\"media\": \"sensor\"") && Has(sidecar, "\"width\": 240, \"height\": 320");
    std::printf("Host view: saved %d (%llu allocations): %ux%u, every pixel within 1 code of the stored sample converted here: %s (worst %d, %u off); hash %016llx, "
                "media_query_snapshot %016llx (source %u, %ux%u, sequence %u); sidecar %s, objects %zu (telemetry %zu)\n", saved,
                static_cast<unsigned long long>(allocations), bmp.width, bmp.height, parsed && !off ? "yes" : "NO", worst, off, static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(s.frame_hash), s.source, s.width, s.height, s.sequence, fields ? "right" : "WRONG", ids.size(), telemetry_ids.size());
    CHECK(parsed && bmp.width == 240 && bmp.height == 320 && off == 0 && worst <= 1);
    CHECK(s.status == MEDIA_OK && s.source == MEDIA_SNAPSHOT_VIEW && s.frame_hash == hash && s.width == 240 && s.height == 320 && s.sequence == 1 && std::strcmp(s.path, path) == 0);
    CHECK(fields && !telemetry_ids.empty() && ids == telemetry_ids && allocations == 0);
    // "take snapshot": a free name in the snapshot folder (here the working directory), the next sequence.
    CHECK(media_engine_set_snapshot_folder(e, ".") == MEDIA_OK);
    const auto commanded = media_engine_command(e, "take snapshot");
    media_snapshot_t second{}; second.struct_size = sizeof(second);
    CHECK(commanded == MEDIA_OK && media_query_snapshot(e, &second) == MEDIA_OK);
    std::vector<std::uint8_t> file2; Bmp bmp2;
    const bool named = second.sequence == 2 && std::strstr(second.path, "sensor_0h00m00.766s") != nullptr && ReadAll(second.path, file2) && ParseBmp(file2, bmp2) &&
                       bmp2.rgb == bmp.rgb;
    std::printf("Host view: \"take snapshot\" %d -> %s (sequence %u, %s)\n", commanded, second.path, second.sequence, named ? "the same picture" : "WRONG");
    CHECK(named);
    Remove(path); Remove(SidecarOf(path).c_str()); Remove(second.path); Remove(SidecarOf(second.path).c_str());
    media_engine_destroy(e);
}

#if defined(_WIN32)
// The GPU image with shadow lift, a 2x zoom and a 3-degree leveling turn: the snapshot is that image.
void TestGpuImage() {
    std::vector<unsigned char> storage(media_engine_storage_bytes() + media_engine_storage_alignment());
    media_engine_config_t config; media_engine_config_init(&config);
    config.playback = MEDIA_PLAYBACK_HOST; config.audio_channels = 0; config.modules = 0;
    media_engine_t* e = nullptr;
    if (media_engine_create(&config, Aligned(storage.data()), media_engine_storage_bytes(), &e) != MEDIA_OK) { CHECK(false); return; }
    media_sensor_config_t s; media_sensor_config_init(&s);
    s.width = kW; s.height = kH; s.options = MEDIA_SENSOR_GPU; s.output_width = kW; s.output_height = kH;
    const auto status = media_engine_open_sensor(e, &s);
    if (status == MEDIA_ERROR_UNSUPPORTED) { std::puts("GPU snapshot: no hardware Direct3D 12 device, skipped"); media_engine_destroy(e); return; }
    CHECK(status == MEDIA_OK);
    media_picture_controls_t controls; media_picture_controls_init(&controls); controls.shadow_lift = 0.5f;
    CHECK(media_engine_set_picture_controls(e, &controls) == MEDIA_OK);
    CHECK(media_engine_command(e, "zoom in") == MEDIA_OK);
    CHECK(media_engine_set_orientation(e, 0, 3.0f) == MEDIA_OK);
    media_sensor_frame_t frame{}; frame.struct_size = sizeof(frame); frame.format = MEDIA_PIXEL_NV12; frame.width = kW; frame.height = kH;
    frame.planes[0] = g_y; frame.planes[1] = g_uv; frame.strides[0] = frame.strides[1] = kW;
    media_gpu_image_t image{}; image.struct_size = sizeof(image);
    bool ready = false;
    for (unsigned f = 0; f < 4; ++f) {
        Draw(f); frame.timestamp_100ns = static_cast<std::int64_t>(f) * 333333;
        CHECK(media_engine_ingest_video_frame(e, &frame) == MEDIA_OK);
        for (const auto start = GetTickCount64(); GetTickCount64() - start < 3000;) {
            if (media_query_gpu_image(e, &image) == MEDIA_OK && image.sequence >= f + 1) { ready = true; break; }
            SwitchToThread();
        }
    }
    CHECK(ready && (image.flags & MEDIA_GPU_GRADED) != 0);
    const char* path = "snapshot_tests_gpu.bmp";
    allocation_probe::Begin();
    const auto saved = media_engine_save_snapshot(e, path);
    const auto allocations = allocation_probe::End();
    std::vector<std::uint8_t> file; Bmp bmp;
    const bool parsed = saved == MEDIA_OK && ReadAll(path, file) && ParseBmp(file, bmp);
    bool same = parsed && bmp.width == image.width && bmp.height == image.height;
    for (unsigned y = 0; same && y < image.height; ++y)
        for (unsigned x = 0; same && x < image.width; ++x)
            for (unsigned k = 0; k < 3; ++k) same = same && bmp.rgb[(std::size_t{y} * image.width + x) * 3 + k] == image.rgba[std::size_t{y} * image.stride + 4 * x + k];
    media_snapshot_t r{}; r.struct_size = sizeof(r);
    CHECK(media_query_snapshot(e, &r) == MEDIA_OK);
    const auto hash = video::SnapshotEncoder::HashRgba(image.rgba, image.stride, image.width, image.height);
    std::vector<std::uint8_t> sidecar_bytes; const bool sidecar_read = ReadAll(SidecarOf(path).c_str(), sidecar_bytes);
    const std::string sidecar(sidecar_bytes.begin(), sidecar_bytes.end());
    const bool fields = sidecar_read && Json::Valid(sidecar) && Has(sidecar, "\"source\": \"gpu-image\"") && Has(sidecar, "\"shadow_lift\": 0.500") &&
                        Has(sidecar, "\"level_degrees\": 3.0") && Has(sidecar, "\"zoom\": 2.000") && Has(sidecar, HashText(hash).c_str());
    std::printf("GPU image: saved %d (%llu allocations): %ux%u, every byte of R G B the GPU image's: %s; hash %016llx (the image's %016llx), source %u; sidecar %s\n",
                saved, static_cast<unsigned long long>(allocations), bmp.width, bmp.height, same ? "yes" : "NO", static_cast<unsigned long long>(r.frame_hash),
                static_cast<unsigned long long>(hash), r.source, fields ? "right" : "WRONG");
    CHECK(same && r.frame_hash == hash && r.source == MEDIA_SNAPSHOT_GPU_IMAGE && fields && allocations == 0);
    Remove(path); Remove(SidecarOf(path).c_str());
    media_engine_destroy(e);
}
// The windowed engine (hidden) on the two-targets fixture.
struct Script {
    media_engine_t* e = nullptr;
    unsigned frames = 0;
    int zoom = -1, first = -1, busy = -1, second = -1;
    bool done1 = false, done2 = false, asked2 = false;
    media_snapshot_t one{}, two{};
    std::uint64_t allocations = 0;
    int on_window_thread = -1;                 // API 17: snapshot_to_memory from the window's own thread (refused).
};
Script g_script;
void OnFrame(void* context, const media_frame_signature_t*) {
    auto& s = *static_cast<Script*>(context);
    ++s.frames;
    allocation_probe::Begin();
    if (s.frames == 20) { (void)media_engine_set_snapshot_folder(s.e, "."); s.zoom = media_engine_command(s.e, "zoom in"); }
    if (s.frames == 30) { s.first = media_engine_command(s.e, "take snapshot"); s.busy = media_engine_command(s.e, "take snapshot"); }
    media_snapshot_t q{}; q.struct_size = sizeof(q);
    if (s.frames > 30 && !s.done1 && media_query_snapshot(s.e, &q) == MEDIA_OK && q.sequence == 1) { s.done1 = true; s.one = q; }
    if (s.done1 && !s.asked2 && s.frames > 40) { s.asked2 = true; s.second = media_engine_save_snapshot(s.e, "snapshot_tests_window.bmp"); }
    if (s.asked2 && !s.done2 && media_query_snapshot(s.e, &q) == MEDIA_OK && q.sequence == 2) { s.done2 = true; s.two = q; }
    if (s.frames == 25) {
        static std::uint8_t tiny[64];
        media_snapshot_buffer_t b{}; b.struct_size = sizeof(b); b.format = MEDIA_PIXELS_BGRA8; b.pixels = tiny; b.capacity_bytes = sizeof(tiny);
        s.on_window_thread = media_engine_snapshot_to_memory(s.e, &b);
    }
    s.allocations += allocation_probe::End();
}
void TestWindow(const char* fixture) {
    if (!fixture || !*fixture) { std::puts("Window: no fixture given, skipped"); return; }
    static std::vector<unsigned char> storage;
    storage.assign(media_engine_storage_bytes() + media_engine_storage_alignment(), 0);
    media_engine_config_t config; media_engine_config_init(&config);
    config.visible = 0; config.maximum_seconds = 4; config.wake_policy = MEDIA_WAKE_DISABLED;
    config.on_frame = &OnFrame; config.on_frame_context = &g_script;
    media_engine_t* e = nullptr;
    CHECK(media_engine_create(&config, Aligned(storage.data()), media_engine_storage_bytes(), &e) == MEDIA_OK);
    if (!e) return;
    g_script = {}; g_script.e = e;
    std::puts("Window: playing the fixture hidden (4 s)");
    CHECK(media_engine_open(e, fixture) == MEDIA_OK);
    // API 17: a snapshot into memory from another thread while it plays (the window's thread makes it): the size first, then the image.
    static std::vector<std::uint8_t> pixels(8u << 20);
    media_snapshot_buffer_t small{}, image{}; int small_status = -1, image_status = -1;
    std::thread memory([&] {
        while (g_script.frames < 60) Sleep(5);
        small.struct_size = sizeof(small); small.format = MEDIA_PIXELS_RGBA8; small.pixels = pixels.data(); small.capacity_bytes = 16;
        small_status = media_engine_snapshot_to_memory(e, &small);
        image = small; image.capacity_bytes = pixels.size();
        image_status = media_engine_snapshot_to_memory(e, &image);
    });
    CHECK(media_engine_run(e) == MEDIA_OK);
    memory.join();
    {
        unsigned opaque = 0; for (std::uint32_t k = 0; k < image.width * image.height; ++k) opaque += pixels[4 * k + 3] == 255;
        std::printf("Window, snapshot to memory: from the window's thread %d (refused), too small %d (%ux%u asked), into %zu bytes %d: %ux%u, hash %016llx, %u opaque\n",
                    g_script.on_window_thread, small_status, small.width, small.height, pixels.size(), image_status, image.width, image.height,
                    static_cast<unsigned long long>(image.frame_hash), opaque);
        CHECK(g_script.on_window_thread == MEDIA_ERROR_STATE && small_status == MEDIA_ERROR_STORAGE && small.width > 0 && image_status == MEDIA_OK);
        CHECK(image.width == small.width && image.height == small.height && image.frame_hash != 0 && opaque == image.width * image.height);
    }
    media_engine_stats_t stats{};
    CHECK(media_engine_stats(e, &stats) == MEDIA_OK);
    const auto& s = g_script;
    const auto check = [](const media_snapshot_t& r, bool zoomed, const char* what) {
        std::vector<std::uint8_t> file; Bmp bmp;
        const bool parsed = ReadAll(r.path, file) && ParseBmp(file, bmp);
        const auto hash = parsed ? Fnv(bmp.rgb.data(), bmp.rgb.size()) : 0;
        std::vector<std::uint8_t> sidecar_bytes; const bool read = ReadAll(SidecarOf(r.path).c_str(), sidecar_bytes);
        const std::string sidecar(sidecar_bytes.begin(), sidecar_bytes.end());
        const bool fields = read && Json::Valid(sidecar) && Has(sidecar, "\"source\": \"window\"") && Has(sidecar, HashText(hash).c_str()) &&
                            Has(sidecar, zoomed ? "\"zoom\": 2.000" : "\"zoom\": 1.000") && Has(sidecar, "\"objects\": [") && Has(sidecar, "playback_two_targets");
        std::printf("Window, %s: %s, %ux%u (record %ux%u), status %d, source %u, the file's hash %s the record's, sidecar %s\n", what, r.path, bmp.width, bmp.height,
                    r.width, r.height, r.status, r.source, hash == r.frame_hash ? "is" : "IS NOT", fields ? "right" : "WRONG");
        return parsed && bmp.width == r.width && bmp.height == r.height && bmp.width > 0 && hash == r.frame_hash && r.status == MEDIA_OK && r.source == MEDIA_SNAPSHOT_WINDOW && fields;
    };
    const bool one = s.done1 && check(s.one, true, "\"take snapshot\"");
    const bool two = s.done2 && check(s.two, true, "save_snapshot to a path");
    std::printf("Window: %u pictures; \"zoom in\" %d, \"take snapshot\" %d, again at once %d (refused: one on its way), save_snapshot %d; %u presented, %u dropped, %u threads; "
                "%llu allocations in the commands\n", s.frames, s.zoom, s.first, s.busy, s.second, stats.presented, stats.dropped, stats.threads,
                static_cast<unsigned long long>(s.allocations));
    CHECK(s.zoom == MEDIA_OK && s.first == MEDIA_OK && s.busy == MEDIA_ERROR_REFUSED && s.second == MEDIA_OK);
    CHECK(one && two && std::strcmp(s.two.path, "snapshot_tests_window.bmp") == 0 && std::strstr(s.one.path, "playback_two_targets_") != nullptr);
    CHECK(stats.dropped == 0 && stats.presented > 60 && s.allocations == 0);
    for (const auto* r : {&s.one, &s.two}) if (r->path[0]) { Remove(r->path); Remove(SidecarOf(r->path).c_str()); }
    CHECK(media_engine_close(e) == MEDIA_OK);
    media_engine_destroy(e);
}
#endif
}

int main(int argc, char** argv) {
    char fixture[1024]{};
#if defined(_WIN32)
    {
        // The SDK takes UTF-8; argv is in the ANSI code page ("Développement" would not survive it): the wide command line.
        int count = 0;
        wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (wide && count > 1) (void)WideCharToMultiByte(CP_UTF8, 0, wide[1], -1, fixture, sizeof(fixture), nullptr, nullptr);
        if (wide) LocalFree(wide);
    }
#else
    if (argc > 1) std::strncpy(fixture, argv[1], sizeof(fixture) - 1);
#endif
    (void)argc; (void)argv;
    std::puts("Snapshot tests"); // stdout's buffer before the first probe (MSVC Debug counts the CRT's).
    TestEncoder();
    TestSidecar();
    TestCommands();
    TestHostView();
#if defined(_WIN32)
    TestGpuImage();
    TestWindow(fixture);
#endif
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "snapshot tests passed", failures);
    return failures ? 1 : 0;
}
