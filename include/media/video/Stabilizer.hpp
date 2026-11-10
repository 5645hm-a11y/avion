#pragma once
#include "media/video/Perception.hpp"
#include <cstdint>

namespace media::video {
// Electronic image stabilisation from an IMU (docs/picture.md, "Stabilisation"), portable, no allocation.
// The camera's rotation since the previous picture (EgoMotion, the same signs as Perception's) is accumulated; its slow part
// (a second-order Butterworth low-pass of delay kSeconds, 2026-11-04: at the same lag on a pan a one-pole left 19 % of a 2.3 Hz
// shake, this 8 %; each picture's interval solved exactly, so any frame rate) is the rotation the operator means (a pan, a tilt,
// a banked turn), the rest is shake. The view
// is moved with the shake, by what it does to the picture's centre (focal x angle), so the scene under the view holds still
// while pans pass through. The view then shows a crop (kZoom at least) and moves inside the picture's margin; a shake larger
// than the margin is held at its edge.
// Roll (2026-10-27; docs/picture.md, "Roll"): the camera rolling by r turns the scene by -r about the picture's centre; the
// view turns with the roll's shake (its fast part), and its centre turns about the picture's centre with it, so a horizon held
// level by the operator stays level through the jitter, while a slow bank (a drone's turn) passes through like a pan. The
// turned view must stay inside the picture: the turn is limited to what the margin holds (kMaxRoll at most), then the centre
// is kept where the turned view's corners fit.
class Stabilizer final {
public:
    static constexpr float kZoom = 1.25f;      // The least zoom while stabilising: a 10 % margin on every side.
    static constexpr double kSeconds = 0.35;   // The low-pass's delay: rotation slower than ~0.6 Hz is meant.
    static constexpr float kMaxRoll = 0.2f;    // Radians (11.5 degrees): the most the view turns.
    // `horizontal_fov_radians` 0: 90 degrees (the SDK's default).
    void Configure(unsigned width, unsigned height, float horizontal_fov_radians) noexcept;
    void Reset() noexcept;
    // One picture: its time and the camera's rotation since the previous one (ignored when not valid).
    void Push(std::int64_t pts_100ns, const EgoMotion& ego) noexcept;
    // The same for pictures in decode order (2026-11-04: the windowed engine measures each one before converting it): `ego` is
    // the motion since the picture pushed before it, whatever its time. A picture earlier than the newest (a B-picture after its
    // successor) leaves the low-pass alone and takes its value at its own time, between the two pictures around it.
    void PushDecoded(std::int64_t pts_100ns, const EgoMotion& ego) noexcept;
    // The view's centre moved by the shake, as fractions of the picture (+x right, +y down), before any margin.
    [[nodiscard]] float ShiftX() const noexcept { return shift_x_; }
    [[nodiscard]] float ShiftY() const noexcept { return shift_y_; }
    // The view's turn with the roll's shake, radians in picture coordinates (+x right, +y down: positive turns clockwise on
    // the screen), before the margin's limit.
    [[nodiscard]] float Turn() const noexcept { return turn_; }
    // The view to show: zoom at least kZoom, its centre (view_x, view_y) moved by the shift and kept inside the picture.
    void Apply(float& zoom, float& view_x, float& view_y) const noexcept;
    // The same with the roll: `roll` is the view's turn (the view shows the picture turned by it about the view's centre).
    void Apply(float& zoom, float& view_x, float& view_y, float& roll) const noexcept;
    [[nodiscard]] double Focal() const noexcept { return focal_; } // Pixels per radian at the centre.
private:
    unsigned width_ = 0, height_ = 0;
    double focal_ = 0;                       // Pixels per radian at the centre.
    double yaw_ = 0, pitch_ = 0, roll_ = 0;  // Accumulated.
    double smooth_yaw_ = 0, smooth_pitch_ = 0, smooth_roll_ = 0;
    double rate_yaw_ = 0, rate_pitch_ = 0, rate_roll_ = 0;   // The low-pass's other state: its rates, per second.
    std::int64_t last_ = INT64_MIN;
    float shift_x_ = 0, shift_y_ = 0, turn_ = 0;
    // The low-pass at the newest pictures (PushDecoded): at most kKept, oldest first.
    static constexpr unsigned kKept = 8;
    struct Kept { std::int64_t pts; double yaw, pitch, roll; };
    Kept kept_[kKept]{}; unsigned kept_count_ = 0;
    void Remember() noexcept;
    void Shift(double smooth_yaw, double smooth_pitch, double smooth_roll) noexcept;
};
}
