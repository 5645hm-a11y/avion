// Offline tool (not a test): AVION's tile flow over a recorded sequence of 8-bit luma pictures, for the accuracy check against dense
// ground truth (tools/realworld/eval_flow.py, docs/realworld.md).
//   media_eval_flow <pictures.gray> <width> <height> <pictures> <frame_100ns> <out.bin> [<times.csv> [refine] [pyramid]]
// out.bin: for every picture after the first, kTiles x kTiles records of three float32: valid (0/1), dx, dy in pixels of the picture per
// frame interval (the tile's flow is in pixels a second; it is multiplied by the interval). Tiles are row-major, each 1/16 of the
// picture a side. With `refine`, the vectors are FlowRefiner's (full-resolution Lucas-Kanade from Perception's answer).
// times.csv: per picture, Perception::Push's time and FlowRefiner::Refine's (0 without refine), microseconds.
#include "media/core/linear_memory_arena.hpp"
#include "media/video/FlowRefiner.hpp"
#include "media/video/Perception.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr, "usage: %s pictures.gray width height pictures frame_100ns out.bin [times.csv [refine]]\n", argv[0]);
        return 2;
    }
    using namespace media;
    using Clock = std::chrono::steady_clock;
    const unsigned width = static_cast<unsigned>(std::atoi(argv[2])), height = static_cast<unsigned>(std::atoi(argv[3]));
    const unsigned pictures = static_cast<unsigned>(std::atoi(argv[4]));
    const std::int64_t frame = std::atoll(argv[5]);
    bool refine = false, pyramid = false; // Flags after times.csv: `refine`, `pyramid` (the coarse level, off by default; 2026-11-05).
    for (int a = 8; a < argc; ++a) { if (std::strcmp(argv[a], "refine") == 0) refine = true; if (std::strcmp(argv[a], "pyramid") == 0) pyramid = true; }
    std::FILE* in = std::fopen(argv[1], "rb");
    std::FILE* out = std::fopen(argv[6], "wb");
    std::FILE* times = argc > 7 ? std::fopen(argv[7], "wb") : nullptr;
    if (!in || !out) { std::fprintf(stderr, "cannot open files\n"); return 2; }

    core::LinearMemoryArena arena;
    if (!arena.Initialize(video::Perception::ArenaBytes(width, true) + (16u << 20))) return 1;
    auto* perception = arena.Create<video::Perception>();
    auto* refiner = arena.Create<video::FlowRefiner>();
    if (perception) perception->SetPyramid(pyramid); // Before Configure: it reserves the coarse grids then.
    if (perception) perception->SetShortBaseline(pyramid && std::getenv("MEDIA_SHORT_BASELINE") != nullptr); // Experiment (2026-11-05).
    if (!perception || !refiner || !perception->Configure(arena, width, height, 1.2f) || !refiner->Configure(width, height)) {
        std::fprintf(stderr, "configure failed\n");
        return 1;
    }
    (void)arena.Seal();

    constexpr unsigned kTiles = video::Perception::kTiles;
    const double seconds = static_cast<double>(frame) * 1e-7;
    const std::size_t plane = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> ring(2 * plane);
    std::int64_t pts = 0;
    for (unsigned f = 0; f < pictures; ++f, pts += frame) {
        std::uint8_t* picture = ring.data() + (f % 2) * plane;
        const std::uint8_t* previous = ring.data() + ((f + 1) % 2) * plane;
        if (std::fread(picture, 1, plane, in) != plane) break;
        const auto start = Clock::now();
        (void)perception->Push(picture, width, pts, {});
        const auto pushed = Clock::now();
        if (refine && f > 0) (void)refiner->Refine(*perception, previous, width, picture, width, seconds);
        const auto done = Clock::now();
        if (times)
            std::fprintf(times, "%.2f,%.2f\n", std::chrono::duration<double, std::micro>(pushed - start).count(),
                         std::chrono::duration<double, std::micro>(done - pushed).count());
        if (f == 0) continue;
        for (unsigned t = 0; t < kTiles * kTiles; ++t) {
            const bool valid = refine ? refiner->Valid(t) : perception->FlowValid(t);
            const double vx = refine ? refiner->FlowX(t) : perception->FlowX(t), vy = refine ? refiner->FlowY(t) : perception->FlowY(t);
            const float rec[3] = {valid ? 1.0f : 0.0f, static_cast<float>(vx * seconds), static_cast<float>(vy * seconds)};
            std::fwrite(rec, sizeof rec, 1, out);
        }
    }
    std::fclose(in); std::fclose(out);
    if (times) std::fclose(times);
    return 0;
}
