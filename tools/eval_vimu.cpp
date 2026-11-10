// Offline tool (not a test): runs the virtual IMU over a recorded sequence of 8-bit luma pictures and writes what it measured,
// one line a picture, for tools/realworld/eval_tumvi.py to compare with a real gyroscope (docs/realworld.md).
//   media_eval_vimu <pictures.gray> <width> <height> <horizontal_fov_degrees> <out.csv>
// The input is raw, tightly packed 8-bit luma (ffmpeg -pix_fmt gray -f rawvideo): pictures back to back, in presentation order.
#include "media/core/linear_memory_arena.hpp"
#include "media/video/VirtualImu.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: %s pictures.gray width height fov_degrees out.csv\n", argv[0]);
        return 2;
    }
    const unsigned width = static_cast<unsigned>(std::atoi(argv[2])), height = static_cast<unsigned>(std::atoi(argv[3]));
    const float fov = static_cast<float>(std::atof(argv[4])) * 3.14159265358979f / 180.0f;
    if (width < 32 || height < 32) { std::fprintf(stderr, "bad size\n"); return 2; }
    std::FILE* in = std::fopen(argv[1], "rb");
    std::FILE* out = std::fopen(argv[5], "wb");
    if (!in || !out) { std::fprintf(stderr, "cannot open files\n"); return 2; }

    media::core::LinearMemoryArena arena;
    if (!arena.Initialize(4u << 20)) { std::fprintf(stderr, "arena\n"); return 1; }
    media::video::VirtualImu imu;
    if (!imu.Configure(arena, width, height, fov)) { std::fprintf(stderr, "configure\n"); return 1; }
    (void)arena.Seal();

    std::vector<std::uint8_t> picture(static_cast<std::size_t>(width) * height);
    std::fprintf(out, "index,valid,dx,dy,turn,tiles,inliers,residual\n");
    unsigned index = 0;
    while (std::fread(picture.data(), 1, picture.size(), in) == picture.size()) {
        const auto& r = imu.Push(picture.data(), width);
        std::fprintf(out, "%u,%d,%.6f,%.6f,%.8f,%u,%u,%.6f\n", index, r.valid ? 1 : 0, r.dx, r.dy, r.turn, r.tiles, r.inliers, r.residual);
        ++index;
    }
    std::fclose(in); std::fclose(out);
    std::printf("%u pictures, %u with a valid motion\n", imu.Pictures(), imu.Measured());
    return 0;
}
