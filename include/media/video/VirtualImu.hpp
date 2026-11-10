#pragma once
#include "media/core/linear_memory_arena.hpp"
#include "media/video/Perception.hpp"
#include <cstdint>

namespace media::video {
// What the virtual IMU measured between the previous picture and this one (VirtualImu::Push).
struct VirtualImuResult {
    bool valid = false;           // Enough of the background agreed (kMinInliers tiles); else no motion is reported.
    float dx = 0, dy = 0;         // The scene's motion at the picture's centre, samples of the picture (+x right, +y down).
    float turn = 0;               // The scene's turn about the centre, radians (+: clockwise on the picture, y down).
    std::uint32_t tiles = 0;      // Tiles measured (textured, away from the objects).
    std::uint32_t inliers = 0;    // Of them, the ones the motion explains (the background).
    float residual = 0;           // Their rms disagreement with it, samples of the picture.
};

// A vision-derived IMU for pictures without one (docs/picture.md, "The virtual IMU", 2026-11-03), portable, no allocation
// after Configure: a file, a webcam, a camera whose IMU the host does not pass. The camera's rotation since the previous picture
// is read off the background's motion, as an IMU would have reported it, so that everything driven by one (Stabilizer,
// vibrometry's fovea, the event camera's references, perception's flow) works the same.
// - The picture as a working grid of at most kMaxWidth x kMaxHeight cells (box means over `scale` x `scale` samples, sixteenths
//   of a code: 16 bits a cell, 2026-11-04), kept for the next picture.
// - The global motion first: the half-resolution grids' mean-removed sum of absolute differences over +-kSearch half cells
//   about the last motion (a shake of a dozen cells a picture is found), then per tile (kTilesX x kTilesY) a Lucas-Kanade
//   refinement from there (Gauss-Newton on the bilinear previous grid, the tile's own gradients, its mean difference taken
//   out: a flicker or a pulse is not motion), at most kIterations steps. A tile counts when it is textured (its gradient
//   tensor's smaller eigenvalue at least kMinTexture codes^2 a cell) and converged; the tiles on the objects the caller names
//   (perception's moving and salient objects: "the median non-object tile") are left out while kMinInliers others remain.
// - The background: the tiles' median motion, then the least-squares translation and turn (dx - turn y, dy + turn x about the
//   centre) through the tiles within max(kInlier, 3 x their median deviation) of it, fitted twice. An object larger than
//   the background, or a picture without texture, gives no motion (valid false): the camera is then taken as still.
// Ego() converts it to EgoMotion: yaw = -dx / focal, pitch = dy / focal, roll = -turn (the IMU's convention: Perception's
// header), focal from the field of view. Roll comes from the tiles' turn (every picture, any scene), not from the horizon's
// lines (HorizonLevel needs straight lines in view and settles over a dozen pictures: it levels the picture, slowly).
class VirtualImu final {
public:
    static constexpr unsigned kMaxWidth = 256, kMaxHeight = 192;
    static constexpr unsigned kTilesX = 8, kTilesY = 6;
    static constexpr int kSearch = 6;                 // Half-resolution cells either way around the last motion.
    static constexpr unsigned kIterations = 6;
    static constexpr float kMinTexture = 4.0f;        // Codes^2 per cell: the tile's weaker gradient direction.
    static constexpr float kInlier = 0.35f;           // Cells: a background tile's least allowed disagreement.
    static constexpr unsigned kMinInliers = 8;
    [[nodiscard]] static std::uint64_t ArenaBytes() noexcept { return 2ull * (2 * kMaxWidth * kMaxHeight + (kMaxWidth / 2) * (kMaxHeight / 2) * 2) + 256; }
    // The pictures' size (8-bit luma, at least 32 x 32), the camera's horizontal field of view (0: 90 degrees).
    [[nodiscard]] bool Configure(core::LinearMemoryArena& arena, unsigned width, unsigned height, float horizontal_fov_radians) noexcept;
    [[nodiscard]] bool Configured() const noexcept { return grid_[0] != nullptr; }
    void Reset() noexcept;  // A cut, a seek: the next picture has nothing to compare with.
    // The next picture's motion is not the last one's: the search starts about no motion (pictures in decode order: going from a
    // picture to the B-picture before it reverses the motion).
    void ForgetMotion() noexcept { last_x_ = last_y_ = 0; }
    void Release() noexcept { grid_[0] = grid_[1] = half_[0] = half_[1] = nullptr; previous_ = false; } // Its arena is gone.
    // One picture in presentation order; `objects` (fractions of the picture: their boxes) are left out.
    const VirtualImuResult& Push(const std::uint8_t* luma, unsigned stride, const PerceivedObject* objects = nullptr, unsigned object_count = 0) noexcept;
    [[nodiscard]] const VirtualImuResult& Result() const noexcept { return result_; }
    [[nodiscard]] EgoMotion Ego() const noexcept;
    [[nodiscard]] unsigned Scale() const noexcept { return scale_; }
    [[nodiscard]] std::uint32_t Pictures() const noexcept { return pictures_; } // Pushed since Configure.
    [[nodiscard]] std::uint32_t Measured() const noexcept { return measured_; } // Of them, with a valid motion.
private:
    void Decimate(const std::uint8_t* luma, unsigned stride, std::uint16_t* out) const noexcept;
    void Halve(const std::uint16_t* grid, std::uint16_t* out) const noexcept;
    unsigned width_ = 0, height_ = 0, scale_ = 1, columns_ = 0, rows_ = 0;
    double focal_ = 0;                // Pixels per radian at the centre.
    std::uint16_t* grid_[2]{};        // This picture's and the previous one's, columns_ x rows_ (codes, Q4).
    std::uint16_t* half_[2]{};        // Their half-resolution grids.
    unsigned current_ = 0;
    bool previous_ = false;           // grid_[current_ ^ 1] holds the previous picture.
    float last_x_ = 0, last_y_ = 0;   // The last motion found (cells): where the search starts.
    VirtualImuResult result_{};
    std::uint32_t pictures_ = 0, measured_ = 0;
};
}
