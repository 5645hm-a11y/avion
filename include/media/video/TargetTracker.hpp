#pragma once
#include <cstdint>

namespace media::video {
// A picture's luma at coarse resolution: one cell per `scale` x `scale` samples of the crop, their mean (8-bit code).
struct LumaGrid { const std::uint8_t* cells = nullptr; unsigned width = 0, height = 0, stride = 0; };
// The grid's cell size for a crop: the smallest whole scale that fits it in kMaxGridWidth x kMaxGridHeight cells.
[[nodiscard]] unsigned GridScale(unsigned width, unsigned height) noexcept;
// Cells of `luma` (8-bit samples): ceil(width / scale) x ceil(height / scale), each the rounded mean of the samples it
// covers (edge cells cover fewer). The renderer's grid pass does the same arithmetic on the GPU.
void MakeGrid(const std::uint8_t* luma, unsigned stride, unsigned width, unsigned height, unsigned scale, std::uint8_t* out, unsigned out_stride) noexcept;

enum class TrackState : std::uint8_t { Off, Tracking, Lost };
struct TrackStatus {
    TrackState state = TrackState::Off;
    float x = 0.5f, y = 0.5f;   // The target's centre (smoothed), as fractions of the picture (0 left/top, 1 right/bottom).
    float match_x = 0.5f, match_y = 0.5f; // This picture's match (before smoothing), the same units.
    float vx = 0, vy = 0;       // The filter's velocity, fractions of the picture per picture (0 while lost).
    float confidence = 0;       // The best match's normalised cross-correlation this picture (-1..1).
    unsigned lost = 0;          // Pictures since the target was last seen (0 while tracking).
    bool reacquired = false;    // Seen again this picture after being lost.
};
// Autonomous target tracking for the zoom and the audio focus (docs/picture.md, "Target tracking"; AI tasks A11, A35).
// Lock takes a kPatch x kPatch-cell template of the grid around a point (the centre of the current view). Each picture:
// - the position is predicted with an alpha-beta (constant-velocity) filter;
// - the template is matched by zero-mean normalised cross-correlation (ZNCC), weighted by a raised-cosine window over the
//   patch (the background at its edges counts little), at every whole-cell offset within kSearch cells
//   of the prediction (2 kSearch while lost), and the best offset refined to a fraction of a cell by a parabola through its
//   neighbours (independently in x and y);
// - a match of at least kAccept moves the estimate (alpha 0.7, beta 0.3); the template follows slow changes of the target
//   (blended 10 % toward a match of at least kRefresh, so a turning or brightening target is kept without drifting off it);
// - below kAccept, or on a scene cut, the target is lost: the estimate holds where it was (the view does not jump), the
//   velocity is forgotten, and the search continues around the last position; a match of at least kReacquire takes it back.
// ZNCC ignores brightness and contrast changes of the whole patch. A template with no texture (its standard deviation
// below kMinTexture codes) is refused: there is nothing to follow. Portable; fixed storage (about 2 KB), no allocation,
// one thread.
class TargetTracker final {
public:
    static constexpr unsigned kPatch = 16, kSearch = 10;
    static constexpr float kAccept = 0.6f, kReacquire = 0.75f, kRefresh = 0.92f, kMinTexture = 2.0f;
    static constexpr float kAlpha = 0.7f, kBeta = 0.3f;
    // Locks onto the patch centred at (x, y) (fractions of the picture). False when the grid is smaller than the patch or
    // the patch has no texture; the tracker is then Off.
    [[nodiscard]] bool Lock(const LumaGrid& grid, float x, float y) noexcept;
    // One picture (in presentation order). `scene_cut`: the picture starts a new shot.
    TrackStatus Update(const LumaGrid& grid, bool scene_cut) noexcept;
    void Stop() noexcept { status_ = {}; }
    [[nodiscard]] const TrackStatus& Status() const noexcept { return status_; }
    [[nodiscard]] bool Active() const noexcept { return status_.state != TrackState::Off; }
private:
    // ZNCC of the template with the grid patch whose top-left cell is (left, top); -2 when it does not fit.
    [[nodiscard]] float Match(const LumaGrid& grid, int left, int top) const noexcept;
    [[nodiscard]] float Mean(const LumaGrid& grid, int left, int top) const noexcept;
    void Take(const LumaGrid& grid, int left, int top, float keep) noexcept;
    float weight_[kPatch * kPatch]{};   // The raised-cosine window.
    float weight_sum_ = 1;
    float template_[kPatch * kPatch]{}; // Zero weighted mean.
    float norm_ = 0;                     // Its weighted norm.
    float cx_ = 0, cy_ = 0, vx_ = 0, vy_ = 0; // Centre and velocity, in cells.
    unsigned grid_width_ = 0, grid_height_ = 0;
    TrackStatus status_{};
};
}
