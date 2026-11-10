#pragma once
#include "media/video/Perception.hpp"
#include <cstddef>
#include <cstdint>

namespace media::video {
// Forensic snapshots (docs/picture.md, "Forensic snapshots", 2026-11-01): the picture as the engine made it, as an uncompressed
// 24-bit BMP, and a sidecar of its telemetry (JSON, UTF-8), portable and without allocation.
// - The image: BITMAPFILEHEADER and BITMAPINFOHEADER (54 bytes; 24 bits, BI_RGB, 2835 pixels a metre), then the rows top to
//   bottom (a negative height: a top-down DIB), each B G R per pixel, padded with zeros to a multiple of 4 bytes.
// - The frame hash: FNV-1a 64 over the image's R G B bytes, pixel by pixel, rows top to bottom (no padding, no alpha): what a
//   reader of the file or of the GPU image (HashRgba) computes the same way.
// - The sidecar ("media-engine-snapshot/1"): the image's name, size and hash, the picture's time, where it came from, the view
//   (zoom, centre, roll, the source's region), the orientation (quarter turns, mirror, leveling, auto-leveling, fit), the
//   horizon, the picture controls, the target and the objects perceived (SnapshotInfo). Numbers in fixed decimals.
struct SnapshotInfo {
    const char* source = "";              // "gpu-image", "view", "window".
    const char* media = nullptr;          // The file (UTF-8), or null: a live sensor.
    std::int64_t pts_100ns = -1;          // The picture's presentation time.
    std::uint32_t sequence = 0;           // This snapshot's number since open (from 1).
    float view_zoom = 1, view_x = 0.5f, view_y = 0.5f, view_roll_degrees = 0; // The view (fractions of the picture).
    std::uint32_t region[4]{};            // The source's region shown (samples: left, top, right, bottom).
    std::uint32_t quarter_turns = 0; bool mirrored = false; float level_degrees = 0; bool auto_level = false; bool fitted = false;
    bool horizon_known = false; float horizon_degrees = 0;
    float brightness = 0, contrast = 1, saturation = 1, gamma = 1, shadow_lift = 0, micro_motion = 1;
    const char* reconstruction = nullptr; // "neural", "edge", "bilinear" (null: none said).
    const char* comparison = nullptr;     // "original-enhanced", "pinned-live" (null: none).
    std::uint32_t target_id = 0;          // The object target tracking follows (0: none).
    PerceivedObject objects[kMaxPerceivedObjects]{};
    std::uint32_t object_count = 0;
};
// A destination for bytes (a file on every platform: platform::OutputFile::Sink; a buffer in tests).
struct SnapshotSink { void* context = nullptr; bool (*write)(void* context, const void* bytes, std::size_t count) noexcept = nullptr; };
// Fills `rgb` (width x 3 bytes: R G B) with row `y` (0: the top) of an image.
using SnapshotRows = void (*)(const void* context, unsigned y, unsigned width, std::uint8_t* rgb) noexcept;
// An RGBA image's rows (the GPU's readbacks: 8-bit R G B A, `stride` bytes a row). Context for RgbaRows.
struct RgbaImage { const std::uint8_t* rgba = nullptr; std::size_t stride = 0; };
// The software view (host mode without the GPU path): `region` of an 8-bit 4:2:0 picture (limited-range BT.709) shown through
// `display` (video::Orientation::Display: display = D stored about the centres; an odd turn's image is the region's height
// across). Each pixel the sample under it (no scaling), its chroma the 2x2 block's. Context for ViewRows.
struct ViewImage {
    const std::uint8_t *luma = nullptr, *cb = nullptr, *cr = nullptr; std::uint32_t luma_stride = 0, chroma_stride = 0;
    std::uint32_t left = 0, top = 0, right = 0, bottom = 0; int display[4]{1, 0, 0, 1};
    [[nodiscard]] unsigned Width() const noexcept { return display[0] == 0 ? bottom - top : right - left; }
    [[nodiscard]] unsigned Height() const noexcept { return display[0] == 0 ? right - left : bottom - top; }
};
// A caller's buffer for a snapshot (API 17, media_engine_snapshot_to_memory): 8-bit B G R A (or R G B A), alpha 255, rows `stride`
// bytes apart (0: width x 4), top row first.
struct SnapshotMemory { std::uint8_t* pixels = nullptr; std::uint64_t capacity = 0; std::uint32_t stride = 0; bool rgba = false; };
class SnapshotEncoder final {
public:
    static constexpr unsigned kMaxSide = 8192;
    static constexpr std::size_t kHeaderBytes = 54, kBufferBytes = 64 * 1024;
    // The image as a top-down 24-bit BMP to `sink`, its rows from `rows`. `hash`: the frame hash. False: a side of 0 or over
    // kMaxSide, or a write that failed.
    bool WriteBmp(unsigned width, unsigned height, SnapshotRows rows, const void* context, const SnapshotSink& sink, std::uint64_t& hash) noexcept;
    // The image into `memory` (API 17): the rows WriteBmp writes, the same frame hash. False: a side of 0 or over kMaxSide, or a
    // buffer too small (stride under width x 4, or capacity under stride x height).
    bool WriteMemory(unsigned width, unsigned height, SnapshotRows rows, const void* context, const SnapshotMemory& memory, std::uint64_t& hash) noexcept;
    static void RgbaRows(const void* context, unsigned y, unsigned width, std::uint8_t* rgb) noexcept;
    static void ViewRows(const void* context, unsigned y, unsigned width, std::uint8_t* rgb) noexcept;
    // The frame hash of an RGBA image (the GPU image; tests).
    [[nodiscard]] static std::uint64_t HashRgba(const std::uint8_t* rgba, std::size_t stride, unsigned width, unsigned height) noexcept;
    static constexpr std::uint64_t kHashStart = 14695981039346656037ull, kHashPrime = 1099511628211ull;
private:
    std::uint8_t row_[kMaxSide * 3]{};
    std::uint8_t buffer_[kBufferBytes]{};
};
// The view's region of a width x height picture (the windowed engine's and the GPU path's rule): the picture's shape at 1/zoom,
// on even samples, at least 16, centred at (x, y) (fractions), inside the picture.
void ViewRegion(unsigned width, unsigned height, float zoom, float x, float y, std::uint32_t region[4]) noexcept;
// The sidecar of `info` for an image `image_name` (its file name), width x height, with `hash`; the bytes written (no
// terminator counted; one is written when there is room), 0 when it does not fit `capacity`.
std::size_t FormatSidecar(const SnapshotInfo& info, const char* image_name, unsigned width, unsigned height, std::uint64_t hash, char* out,
                          std::size_t capacity) noexcept;
// The sidecar's path for an image's: a final ".bmp" (any case) replaced by ".json", else ".json" added. False: too long.
bool SidecarPath(const char* image_path, char* out, std::size_t capacity) noexcept;
// A snapshot's file name: "<stem>_<h>h<mm>m<ss>.<mmm>s.bmp" from the picture's time ("_2" .. before ".bmp" for a later
// attempt), the stem's characters that cannot be in a file name replaced by '_'. False: too long.
bool SnapshotName(const char* stem, std::int64_t pts_100ns, unsigned attempt, char* out, std::size_t capacity) noexcept;
// One snapshot written (the C SDK's media_snapshot_t): where, what, and whether it all went through.
struct SnapshotResult {
    bool written = false;
    std::uint32_t width = 0, height = 0;
    std::int64_t pts_100ns = -1;
    std::uint64_t hash = 0;
    char path[512]{};                     // The image's (UTF-8); the sidecar's is SidecarPath of it.
};
// Writes the image (`rows`, width x height) to `path` (UTF-8, replaced when it exists), or with no path to a free name in
// `folder` (UTF-8, ending with its separator; "" the working directory) from `stem` and the picture's time (SnapshotName, up to
// 99 attempts), then its sidecar. An image whose writing failed is removed. Through platform::OutputFile: no allocation.
bool SaveSnapshot(SnapshotEncoder& encoder, const char* path, const char* folder, const char* stem, unsigned width, unsigned height, SnapshotRows rows,
                  const void* context, const SnapshotInfo& info, SnapshotResult& result) noexcept;
// SaveSnapshot, or with `memory` the image into it instead (no file, no sidecar; `result.path` empty).
bool SaveSnapshotTo(SnapshotEncoder& encoder, const SnapshotMemory* memory, const char* path, const char* folder, const char* stem, unsigned width,
                    unsigned height, SnapshotRows rows, const void* context, const SnapshotInfo& info, SnapshotResult& result) noexcept;
}
