#pragma once
#include "media/video/Perception.hpp"
#include <cstdint>

namespace media::video {
// Full-resolution refinement of Perception's tile flow (2026-11-04; docs/realworld.md, "Tile flow against ground truth"), portable,
// no allocation and no copy: the caller passes the previous picture it already holds.
// Perception matches tiles on its kCells x kCells grid of cell means (a cell is width / 128 pixels: 5 at 640): fast, but on
// Middlebury's real pictures its tiles were 1.0 px off the dense ground truth where OpenCV's pyramidal Lucas-Kanade at the same 256
// points was 0.65. The grid's answer is close enough to start a single-level Lucas-Kanade at full resolution, so no pyramid is
// built and no picture-wide gradient is computed: per tile, a kWindow x kWindow window at the tile's centre, inverse
// compositional (the window's gradients and its 2x2 system once, then at most kSteps bilinear reads of the previous picture),
// mean-removed (a change of brightness is not motion). The same refinement seeded the same way in OpenCV measured 0.64 px.
// A tile keeps Perception's answer (Refined false) when its window would leave either picture, its gradients are degenerate, or the
// refinement walks further than kMaxWalk cells from the grid's answer (the grid's coarser match is then the better guess).
class FlowRefiner final {
public:
    static constexpr unsigned kTiles = Perception::kTiles;
    static constexpr int kWindow = 21;            // Pixels a side, centred on the tile.
    static constexpr unsigned kSteps = 6;
    static constexpr int kCoarse = 1;             // The first pass's sample spacing, pixels (1: no coarse pass).
    static constexpr float kMaxWalk = 6.0f;       // Cells.
    static constexpr float kConverged = 0.01f;    // Pixels: a step this small ends the iteration.

    // The scalar rows everywhere (true) or SSE2 where the target has it (false, the default): the two give the same vectors, bit for bit.
    static void ForceScalar(bool scalar) noexcept;
    // The pictures' size (8-bit luma, as configured in Perception).
    [[nodiscard]] bool Configure(unsigned width, unsigned height) noexcept;
    // After perception.Push(now): refines its valid tiles against `previous`, the picture pushed just before `now`, `seconds` before
    // it. Returns the tiles refined.
    unsigned Refine(const Perception& perception, const std::uint8_t* previous, unsigned previous_stride, const std::uint8_t* now, unsigned stride,
                    double seconds) noexcept;
    // Per tile (row-major, kTiles x kTiles), after Refine: Perception's validity, the motion in pixels a second (refined where Refined).
    [[nodiscard]] bool Valid(unsigned tile) const noexcept { return valid_[tile] != 0; }
    [[nodiscard]] bool Refined(unsigned tile) const noexcept { return refined_[tile] != 0; }
    [[nodiscard]] float FlowX(unsigned tile) const noexcept { return vx_[tile]; }
    [[nodiscard]] float FlowY(unsigned tile) const noexcept { return vy_[tile]; }
    // The tile's centre, pixels (where the window sits).
    [[nodiscard]] float CentreX(unsigned tile) const noexcept;
    [[nodiscard]] float CentreY(unsigned tile) const noexcept;

private:
    bool RefineTile(const std::uint8_t* previous, unsigned previous_stride, const std::uint8_t* now, unsigned stride, int cx, int cy, int spacing, float& dx, float& dy) const noexcept;
    unsigned width_ = 0, height_ = 0;
    std::uint8_t valid_[kTiles * kTiles]{}, refined_[kTiles * kTiles]{};
    float vx_[kTiles * kTiles]{}, vy_[kTiles * kTiles]{};
};
}
