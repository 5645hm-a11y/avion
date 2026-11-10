#include "media/video/Snapshot.hpp"
#include "media/platform/OutputFile.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::video {
namespace {
void Put16(std::uint8_t* p, std::uint32_t v) noexcept { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); }
void Put32(std::uint8_t* p, std::uint32_t v) noexcept { Put16(p, v); Put16(p + 2, v >> 16); }
// limited-range BT.709 Y'CbCr to full-range R'G'B', 8-bit, in 16.16 fixed point (1.164384, 1.792741, 0.213249, 0.532909,
// 2.112402: 255/219 and 255/224 times the matrix's terms), rounded, clamped.
std::uint8_t Clamp8(std::int32_t v) noexcept { return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
// JSON text into a fixed buffer: once something does not fit, nothing more is written and Ok() is false.
struct Text {
    char* out; std::size_t capacity, n = 0; bool ok = true;
    void Raw(const char* s, std::size_t count) noexcept {
        if (!ok || count > capacity - n) { ok = false; return; }
        std::memcpy(out + n, s, count); n += count;
    }
    void Raw(const char* s) noexcept { Raw(s, std::strlen(s)); }
    void Char(char c) noexcept { Raw(&c, 1); }
    void Unsigned(std::uint64_t v) noexcept {
        char digits[24]; unsigned k = 0;
        do { digits[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (k) Char(digits[--k]);
    }
    void Signed(std::int64_t v) noexcept {
        if (v < 0) { Char('-'); Unsigned(static_cast<std::uint64_t>(-(v + 1)) + 1); } else Unsigned(static_cast<std::uint64_t>(v));
    }
    // `decimals` places, rounded half away from zero; not a number: null.
    void Fixed(double v, unsigned decimals) noexcept {
        if (!(v == v) || std::fabs(v) > 1e15) { Raw("null"); return; }
        std::uint64_t scale = 1; for (unsigned i = 0; i < decimals; ++i) scale *= 10;
        const auto scaled = static_cast<std::uint64_t>(std::llround(std::fabs(v) * static_cast<double>(scale)));
        if (v < 0 && scaled) Char('-');
        Unsigned(scaled / scale);
        if (!decimals) return;
        Char('.');
        for (std::uint64_t place = scale / 10, rest = scaled % scale; place; place /= 10) { Char(static_cast<char>('0' + rest / place)); rest %= place; }
    }
    void Bool(bool v) noexcept { Raw(v ? "true" : "false"); }
    // A JSON string: quotes, the backslash and control characters escaped; the rest (UTF-8) as it is.
    void String(const char* s) noexcept {
        Char('"');
        for (; s && *s; ++s) {
            const auto c = static_cast<unsigned char>(*s);
            if (c == '"' || c == '\\') { Char('\\'); Char(static_cast<char>(c)); }
            else if (c < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                Raw("\\u00"); Char(kHex[c >> 4]); Char(kHex[c & 15]);
            } else Char(static_cast<char>(c));
        }
        Char('"');
    }
    void Key(const char* key) noexcept { String(key); Raw(": "); }
};
}
bool SnapshotEncoder::WriteBmp(unsigned width, unsigned height, SnapshotRows rows, const void* context, const SnapshotSink& sink, std::uint64_t& hash) noexcept {
    hash = kHashStart;
    if (!width || !height || width > kMaxSide || height > kMaxSide || !rows || !sink.write) return false;
    const std::uint32_t row_bytes = (3 * width + 3) & ~3u, image_bytes = row_bytes * height;
    std::uint8_t header[kHeaderBytes]{};
    header[0] = 'B'; header[1] = 'M';
    Put32(header + 2, static_cast<std::uint32_t>(kHeaderBytes) + image_bytes); Put32(header + 10, static_cast<std::uint32_t>(kHeaderBytes));
    Put32(header + 14, 40); Put32(header + 18, width); Put32(header + 22, static_cast<std::uint32_t>(-static_cast<std::int32_t>(height))); // Top-down.
    Put16(header + 26, 1); Put16(header + 28, 24); Put32(header + 30, 0); Put32(header + 34, image_bytes);
    Put32(header + 38, 2835); Put32(header + 42, 2835);
    if (!sink.write(sink.context, header, sizeof(header))) return false;
    std::size_t used = 0;
    std::uint64_t h = kHashStart;
    for (unsigned y = 0; y < height; ++y) {
        rows(context, y, width, row_);
        for (unsigned i = 0; i < 3 * width; ++i) { h ^= row_[i]; h *= kHashPrime; }
        if (kBufferBytes - used < row_bytes) { if (!sink.write(sink.context, buffer_, used)) return false; used = 0; }
        std::uint8_t* out = buffer_ + used;
        for (unsigned x = 0; x < width; ++x) { out[3 * x] = row_[3 * x + 2]; out[3 * x + 1] = row_[3 * x + 1]; out[3 * x + 2] = row_[3 * x]; }
        for (unsigned i = 3 * width; i < row_bytes; ++i) out[i] = 0;
        used += row_bytes;
    }
    if (used && !sink.write(sink.context, buffer_, used)) return false;
    hash = h;
    return true;
}
bool SnapshotEncoder::WriteMemory(unsigned width, unsigned height, SnapshotRows rows, const void* context, const SnapshotMemory& memory,
                                  std::uint64_t& hash) noexcept {
    hash = kHashStart;
    if (!width || !height || width > kMaxSide || height > kMaxSide || !rows || !memory.pixels) return false;
    const std::uint64_t stride = memory.stride ? memory.stride : std::uint64_t{width} * 4;
    if (stride < std::uint64_t{width} * 4 || memory.capacity < stride * (height - 1) + std::uint64_t{width} * 4) return false;
    std::uint64_t h = kHashStart;
    for (unsigned y = 0; y < height; ++y) {
        rows(context, y, width, row_);
        for (unsigned i = 0; i < 3 * width; ++i) { h ^= row_[i]; h *= kHashPrime; }
        std::uint8_t* out = memory.pixels + stride * y;
        if (memory.rgba) for (unsigned x = 0; x < width; ++x) { out[4 * x] = row_[3 * x]; out[4 * x + 1] = row_[3 * x + 1]; out[4 * x + 2] = row_[3 * x + 2]; out[4 * x + 3] = 255; }
        else for (unsigned x = 0; x < width; ++x) { out[4 * x] = row_[3 * x + 2]; out[4 * x + 1] = row_[3 * x + 1]; out[4 * x + 2] = row_[3 * x]; out[4 * x + 3] = 255; }
    }
    hash = h;
    return true;
}
void SnapshotEncoder::RgbaRows(const void* context, unsigned y, unsigned width, std::uint8_t* rgb) noexcept {
    const auto& image = *static_cast<const RgbaImage*>(context);
    const std::uint8_t* row = image.rgba + image.stride * y;
    for (unsigned x = 0; x < width; ++x) { rgb[3 * x] = row[4 * x]; rgb[3 * x + 1] = row[4 * x + 1]; rgb[3 * x + 2] = row[4 * x + 2]; }
}
void SnapshotEncoder::ViewRows(const void* context, unsigned y, unsigned width, std::uint8_t* rgb) noexcept {
    const auto& v = *static_cast<const ViewImage*>(context);
    const int* d = v.display;
    const int rw = static_cast<int>(v.right - v.left), rh = static_cast<int>(v.bottom - v.top), ow = static_cast<int>(width), oh = static_cast<int>(v.Height());
    // Twice an output pixel's centre about the image's centre, to the stored picture through D's transpose (D is a turn or a
    // mirror of the axes: its inverse); with even sides every centre lands on a sample's.
    const int v2 = 2 * static_cast<int>(y) + 1 - oh;
    for (int x = 0; x < ow; ++x) {
        const int u2 = 2 * x + 1 - ow;
        const int s2 = d[0] * u2 + d[2] * v2, t2 = d[1] * u2 + d[3] * v2;
        const int sx = std::clamp((rw + s2 - 1) / 2, 0, rw - 1) + static_cast<int>(v.left), sy = std::clamp((rh + t2 - 1) / 2, 0, rh - 1) + static_cast<int>(v.top);
        const std::int32_t l = static_cast<std::int32_t>(v.luma[static_cast<std::size_t>(sy) * v.luma_stride + static_cast<std::size_t>(sx)]) - 16;
        const std::size_t c = static_cast<std::size_t>(sy / 2) * v.chroma_stride + static_cast<std::size_t>(sx / 2);
        const std::int32_t b = static_cast<std::int32_t>(v.cb[c]) - 128, r = static_cast<std::int32_t>(v.cr[c]) - 128;
        const std::int32_t base = 76309 * l + 32768;
        rgb[3 * x] = Clamp8((base + 117489 * r) >> 16);
        rgb[3 * x + 1] = Clamp8((base - 13975 * b - 34925 * r) >> 16);
        rgb[3 * x + 2] = Clamp8((base + 138439 * b) >> 16);
    }
}
std::uint64_t SnapshotEncoder::HashRgba(const std::uint8_t* rgba, std::size_t stride, unsigned width, unsigned height) noexcept {
    std::uint64_t h = kHashStart;
    for (unsigned y = 0; y < height; ++y) {
        const std::uint8_t* row = rgba + stride * y;
        for (unsigned x = 0; x < width; ++x) for (unsigned k = 0; k < 3; ++k) { h ^= row[4 * x + k]; h *= kHashPrime; }
    }
    return h;
}
void ViewRegion(unsigned width, unsigned height, float zoom, float x, float y, std::uint32_t region[4]) noexcept {
    const float z = (std::max)(zoom == zoom ? zoom : 1.0f, 1.0f);
    const auto even = [](double v) { return static_cast<unsigned>(std::lround(v / 2)) * 2u; };
    const unsigned rw = std::clamp(even(width / z), (std::min)(16u, width), width), rh = std::clamp(even(height / z), (std::min)(16u, height), height);
    const double cx = static_cast<double>(x == x ? x : 0.5f) * width, cy = static_cast<double>(y == y ? y : 0.5f) * height;
    region[0] = (std::min)(even(std::clamp(cx - rw / 2.0, 0.0, static_cast<double>(width - rw))), width - rw);
    region[1] = (std::min)(even(std::clamp(cy - rh / 2.0, 0.0, static_cast<double>(height - rh))), height - rh);
    region[2] = region[0] + rw; region[3] = region[1] + rh;
}
std::size_t FormatSidecar(const SnapshotInfo& info, const char* image_name, unsigned width, unsigned height, std::uint64_t hash, char* out,
                          std::size_t capacity) noexcept {
    if (!out || !capacity) return 0;
    Text t{out, capacity - 1};
    t.Raw("{\n  "); t.Key("format"); t.String("media-engine-snapshot/1");
    t.Raw(",\n  "); t.Key("image"); t.String(image_name);
    t.Raw(",\n  "); t.Key("width"); t.Unsigned(width); t.Raw(", "); t.Key("height"); t.Unsigned(height);
    {
        static constexpr char kHex[] = "0123456789abcdef";
        char text[8 + 16 + 1] = "fnv1a64:";
        for (unsigned i = 0; i < 16; ++i) text[8 + i] = kHex[(hash >> (60 - 4 * i)) & 15];
        text[24] = 0;
        t.Raw(",\n  "); t.Key("frame_hash"); t.String(text);
    }
    t.Raw(",\n  "); t.Key("source"); t.String(info.source);
    t.Raw(",\n  "); t.Key("media"); if (info.media) t.String(info.media); else t.String("sensor");
    t.Raw(",\n  "); t.Key("pts_100ns"); t.Signed(info.pts_100ns);
    {
        // The time as h:mm:ss.mmm (of the media; a sensor's own clock).
        const std::int64_t ms = info.pts_100ns > 0 ? info.pts_100ns / 10000 : 0;
        char text[32]; Text clock{text, sizeof(text) - 1};
        clock.Unsigned(static_cast<std::uint64_t>(ms / 3600000)); clock.Char(':');
        const auto two = [&](std::int64_t v) { clock.Char(static_cast<char>('0' + v / 10)); clock.Char(static_cast<char>('0' + v % 10)); };
        two(ms / 60000 % 60); clock.Char(':'); two(ms / 1000 % 60); clock.Char('.');
        clock.Char(static_cast<char>('0' + ms % 1000 / 100)); two(ms % 100);
        text[clock.n] = 0;
        t.Raw(",\n  "); t.Key("time"); t.String(text);
    }
    t.Raw(",\n  "); t.Key("sequence"); t.Unsigned(info.sequence);
    t.Raw(",\n  "); t.Key("view"); t.Raw("{"); t.Key("zoom"); t.Fixed(info.view_zoom, 3); t.Raw(", "); t.Key("x"); t.Fixed(info.view_x, 4);
    t.Raw(", "); t.Key("y"); t.Fixed(info.view_y, 4); t.Raw(", "); t.Key("roll_degrees"); t.Fixed(info.view_roll_degrees, 2);
    t.Raw(", "); t.Key("region"); t.Raw("[");
    for (unsigned i = 0; i < 4; ++i) { if (i) t.Raw(", "); t.Unsigned(info.region[i]); }
    t.Raw("]}");
    t.Raw(",\n  "); t.Key("orientation"); t.Raw("{"); t.Key("quarter_turns"); t.Unsigned(info.quarter_turns); t.Raw(", "); t.Key("mirrored"); t.Bool(info.mirrored);
    t.Raw(", "); t.Key("level_degrees"); t.Fixed(info.level_degrees, 1); t.Raw(", "); t.Key("auto_level"); t.Bool(info.auto_level);
    t.Raw(", "); t.Key("fitted"); t.Bool(info.fitted); t.Raw("}");
    t.Raw(",\n  "); t.Key("horizon"); t.Raw("{"); t.Key("known"); t.Bool(info.horizon_known); t.Raw(", "); t.Key("tilt_degrees");
    if (info.horizon_known) t.Fixed(info.horizon_degrees, 2); else t.Raw("null");
    t.Raw("}");
    t.Raw(",\n  "); t.Key("picture"); t.Raw("{"); t.Key("brightness"); t.Fixed(info.brightness, 3); t.Raw(", "); t.Key("contrast"); t.Fixed(info.contrast, 3);
    t.Raw(", "); t.Key("saturation"); t.Fixed(info.saturation, 3); t.Raw(", "); t.Key("gamma"); t.Fixed(info.gamma, 3);
    t.Raw(", "); t.Key("shadow_lift"); t.Fixed(info.shadow_lift, 3); t.Raw(", "); t.Key("micro_motion"); t.Fixed(info.micro_motion, 2);
    t.Raw(", "); t.Key("reconstruction"); if (info.reconstruction) t.String(info.reconstruction); else t.Raw("null");
    t.Raw(", "); t.Key("comparison"); if (info.comparison) t.String(info.comparison); else t.Raw("null");
    t.Raw("}");
    t.Raw(",\n  "); t.Key("target_track_id"); t.Unsigned(info.target_id);
    t.Raw(",\n  "); t.Key("objects"); t.Raw("[");
    for (unsigned i = 0; i < info.object_count && i < kMaxPerceivedObjects; ++i) {
        const auto& o = info.objects[i];
        t.Raw(i ? ",\n    {" : "\n    {");
        t.Key("track_id"); t.Unsigned(o.track_id); t.Raw(", "); t.Key("x"); t.Fixed(o.x, 4); t.Raw(", "); t.Key("y"); t.Fixed(o.y, 4);
        t.Raw(", "); t.Key("width"); t.Fixed(o.width, 4); t.Raw(", "); t.Key("height"); t.Fixed(o.height, 4);
        t.Raw(", "); t.Key("velocity_x"); t.Fixed(o.vx, 4); t.Raw(", "); t.Key("velocity_y"); t.Fixed(o.vy, 4);
        t.Raw(", "); t.Key("approaching"); t.Bool(o.approaching); t.Raw(", "); t.Key("ttc_seconds"); if (o.approaching) t.Fixed(o.ttc_seconds, 3); else t.Raw("null");
        t.Raw(", "); t.Key("occluded"); t.Bool(o.occluded); t.Raw(", "); t.Key("target"); t.Bool(info.target_id && o.track_id == info.target_id);
        t.Raw(", "); t.Key("tiles"); t.Unsigned(o.tiles); t.Raw(", "); t.Key("age"); t.Unsigned(o.age); t.Raw("}");
    }
    t.Raw(info.object_count ? "\n  ]\n}\n" : "]\n}\n");
    if (!t.ok) return 0;
    out[t.n] = 0;
    return t.n;
}
bool SidecarPath(const char* image_path, char* out, std::size_t capacity) noexcept {
    if (!image_path || !out) return false;
    std::size_t n = std::strlen(image_path);
    const bool bmp = n >= 4 && image_path[n - 4] == '.' && (image_path[n - 3] | 32) == 'b' && (image_path[n - 2] | 32) == 'm' && (image_path[n - 1] | 32) == 'p';
    if (bmp) n -= 4;
    if (n + 6 > capacity) return false;
    std::memcpy(out, image_path, n); std::memcpy(out + n, ".json", 6);
    return true;
}
bool SnapshotName(const char* stem, std::int64_t pts_100ns, unsigned attempt, char* out, std::size_t capacity) noexcept {
    if (!out || !capacity) return false;
    Text t{out, capacity - 1};
    for (const char* c = stem && *stem ? stem : "snapshot"; *c; ++c) {
        const auto u = static_cast<unsigned char>(*c);
        t.Char(u < 0x20 || std::strchr("<>:\"/\\|?*", *c) ? '_' : *c);
    }
    const std::int64_t ms = pts_100ns > 0 ? pts_100ns / 10000 : 0;
    const auto two = [&](std::int64_t v) { t.Char(static_cast<char>('0' + v / 10)); t.Char(static_cast<char>('0' + v % 10)); };
    t.Char('_'); t.Unsigned(static_cast<std::uint64_t>(ms / 3600000)); t.Char('h'); two(ms / 60000 % 60); t.Char('m'); two(ms / 1000 % 60);
    t.Char('.'); t.Char(static_cast<char>('0' + ms % 1000 / 100)); two(ms % 100); t.Char('s');
    if (attempt > 1) { t.Char('_'); t.Unsigned(attempt); }
    t.Raw(".bmp");
    if (!t.ok) return false;
    out[t.n] = 0;
    return true;
}
bool SaveSnapshot(SnapshotEncoder& encoder, const char* path, const char* folder, const char* stem, unsigned width, unsigned height, SnapshotRows rows,
                  const void* context, const SnapshotInfo& info, SnapshotResult& result) noexcept {
    result = {}; result.width = width; result.height = height; result.pts_100ns = info.pts_100ns;
    platform::OutputFile file;
    if (path && *path) {
        const auto n = std::strlen(path);
        if (n >= sizeof(result.path)) return false;
        std::memcpy(result.path, path, n + 1);
        if (!file.Create(result.path, true)) return false;
    } else {
        // A free name: the next attempt when one exists (never replaced).
        const std::size_t f = folder ? std::strlen(folder) : 0;
        char name[256]; bool created = false;
        for (unsigned attempt = 1; attempt <= 99 && !created; ++attempt) {
            if (!SnapshotName(stem, info.pts_100ns, attempt, name, sizeof(name))) return false;
            const auto n = std::strlen(name);
            if (f + n + 1 > sizeof(result.path)) return false;
            if (f) std::memcpy(result.path, folder, f);
            std::memcpy(result.path + f, name, n + 1);
            created = file.Create(result.path, false);
        }
        if (!created) { result.path[0] = 0; return false; }
    }
    std::uint64_t hash = 0;
    const bool image = encoder.WriteBmp(width, height, rows, context, {&file, &platform::OutputFile::WriteTo}, hash);
    if (!file.Close() || !image) { (void)platform::OutputFile::Remove(result.path); return false; }
    result.hash = hash;
    // The sidecar beside it, naming the image by its file name.
    const char* base = result.path;
    for (const char* c = result.path; *c; ++c) if (*c == '/' || *c == '\\') base = c + 1;
    char text[8192], sidecar[sizeof(result.path) + 8];
    const auto bytes = FormatSidecar(info, base, width, height, hash, text, sizeof(text));
    if (!bytes || !SidecarPath(result.path, sidecar, sizeof(sidecar)) || !file.Create(sidecar, true)) return false;
    const bool wrote = file.Write(text, bytes);
    if (!file.Close() || !wrote) return false;
    result.written = true;
    return true;
}
bool SaveSnapshotTo(SnapshotEncoder& encoder, const SnapshotMemory* memory, const char* path, const char* folder, const char* stem, unsigned width,
                    unsigned height, SnapshotRows rows, const void* context, const SnapshotInfo& info, SnapshotResult& result) noexcept {
    if (!memory) return SaveSnapshot(encoder, path, folder, stem, width, height, rows, context, info, result);
    result = {}; result.width = width; result.height = height; result.pts_100ns = info.pts_100ns;
    result.written = encoder.WriteMemory(width, height, rows, context, *memory, result.hash);
    return result.written;
}
}
