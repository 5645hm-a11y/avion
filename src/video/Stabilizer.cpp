#include "media/video/Stabilizer.hpp"
#include <algorithm>
#include <cmath>

namespace media::video {
void Stabilizer::Configure(unsigned width, unsigned height, float fov) noexcept {
    width_ = width; height_ = height;
    const double half = (fov > 0 ? static_cast<double>(fov) : 1.5707963267948966) * 0.5;
    focal_ = width ? 0.5 * width / std::tan(half) : 0;
    Reset();
}
void Stabilizer::Reset() noexcept {
    yaw_ = pitch_ = roll_ = smooth_yaw_ = smooth_pitch_ = smooth_roll_ = rate_yaw_ = rate_pitch_ = rate_roll_ = 0; last_ = INT64_MIN; shift_x_ = shift_y_ = turn_ = 0; kept_count_ = 0;
}
void Stabilizer::Remember() noexcept {
    if (kept_count_ == kKept) { for (unsigned i = 1; i < kKept; ++i) kept_[i - 1] = kept_[i]; --kept_count_; }
    kept_[kept_count_++] = {last_, smooth_yaw_, smooth_pitch_, smooth_roll_};
}
void Stabilizer::Shift(double smooth_yaw, double smooth_pitch, double smooth_roll) noexcept {
    // Yaw to the right moves the scene left by focal x angle; pitch up moves it down: the view goes with it. Roll clockwise
    // turns the scene counter-clockwise on the screen (by -roll in picture coordinates): the view turns with it.
    shift_x_ = static_cast<float>(-focal_ * (yaw_ - smooth_yaw) / width_);
    shift_y_ = static_cast<float>(focal_ * (pitch_ - smooth_pitch) / height_);
    turn_ = static_cast<float>(-(roll_ - smooth_roll));
}
void Stabilizer::Push(std::int64_t pts, const EgoMotion& ego) noexcept {
    if (!width_ || !height_) return;
    if (ego.valid) { yaw_ += ego.yaw; pitch_ += ego.pitch; roll_ += ego.roll; }
    // The low-pass follows by the time since the previous picture (a first picture: it starts where the camera is).
    if (last_ == INT64_MIN || pts <= last_) { smooth_yaw_ = yaw_; smooth_pitch_ = pitch_; smooth_roll_ = roll_; rate_yaw_ = rate_pitch_ = rate_roll_ = 0; }
    else {
        // e'' + 2 a e' + 2 a^2 e = 0 (e: the low-pass less its input, held over the interval; damping 1/sqrt 2, delay 1/a), solved over it.
        const double a = 1 / kSeconds, dt = static_cast<double>(pts - last_) / 1e7, decay = std::exp(-a * dt), co = std::cos(a * dt), si = std::sin(a * dt);
        const auto follow = [&](double& smooth, double& rate, double input) {
            const double e = smooth - input, v = rate;
            smooth = input + decay * (e * co + (v / a + e) * si);
            rate = decay * (v * co - (2 * a * e + v) * si);
        };
        follow(smooth_yaw_, rate_yaw_, yaw_); follow(smooth_pitch_, rate_pitch_, pitch_); follow(smooth_roll_, rate_roll_, roll_);
    }
    last_ = pts;
    Shift(smooth_yaw_, smooth_pitch_, smooth_roll_);
}
void Stabilizer::PushDecoded(std::int64_t pts, const EgoMotion& ego) noexcept {
    if (!width_ || !height_) return;
    if (last_ == INT64_MIN || pts > last_) { Push(pts, ego); Remember(); return; }
    if (ego.valid) { yaw_ += ego.yaw; pitch_ += ego.pitch; roll_ += ego.roll; }
    // The low-pass at this picture's time: linear between the kept pictures around it (the oldest before them).
    unsigned after = 0;
    while (after < kept_count_ && kept_[after].pts < pts) ++after;
    if (after == 0 || after == kept_count_) { const auto& k = kept_[after ? after - 1 : 0]; Shift(k.yaw, k.pitch, k.roll); return; }
    const auto& a = kept_[after - 1]; const auto& b = kept_[after];
    const double f = static_cast<double>(pts - a.pts) / static_cast<double>(b.pts - a.pts);
    Shift(a.yaw + (b.yaw - a.yaw) * f, a.pitch + (b.pitch - a.pitch) * f, a.roll + (b.roll - a.roll) * f);
}
void Stabilizer::Apply(float& zoom, float& view_x, float& view_y) const noexcept {
    zoom = (std::max)(zoom, kZoom);
    const float half = 0.5f / zoom; // Half the view, as a fraction of the picture.
    view_x = std::clamp(view_x + shift_x_, half, 1 - half);
    view_y = std::clamp(view_y + shift_y_, half, 1 - half);
}
void Stabilizer::Apply(float& zoom, float& view_x, float& view_y, float& roll) const noexcept {
    zoom = (std::max)(zoom, kZoom);
    if (!width_ || !height_) { roll = 0; Apply(zoom, view_x, view_y); return; }
    const double w = width_, h = height_;
    // Half the turned view's bounding box, as fractions of the picture's width and height.
    const auto extent = [&](double turn, double& hx, double& hy) {
        const double c = std::fabs(std::cos(turn)), s = std::fabs(std::sin(turn));
        hx = 0.5 / zoom * (c + s * h / w); hy = 0.5 / zoom * (c + s * w / h);
    };
    // The turn: the roll's shake, at most kMaxRoll and what the margin holds (halved until it fits, then refined).
    double turn = std::clamp(static_cast<double>(turn_), -static_cast<double>(kMaxRoll), static_cast<double>(kMaxRoll)), hx = 0, hy = 0;
    extent(turn, hx, hy);
    if (hx > 0.5 || hy > 0.5) {
        double fits = 0, fails = turn;
        for (unsigned i = 0; i < 16; ++i) {
            const double mid = 0.5 * (fits + fails);
            extent(mid, hx, hy);
            if (hx > 0.5 || hy > 0.5) fails = mid; else fits = mid;
        }
        turn = fits; extent(turn, hx, hy);
    }
    // The centre moves with the shake and turns with the scene about the picture's centre, in pixels.
    const double cx = (static_cast<double>(view_x) + shift_x_ - 0.5) * w, cy = (static_cast<double>(view_y) + shift_y_ - 0.5) * h;
    const double tx = cx * std::cos(turn) - cy * std::sin(turn), ty = cx * std::sin(turn) + cy * std::cos(turn);
    view_x = static_cast<float>(std::clamp(0.5 + tx / w, hx, 1 - hx));
    view_y = static_cast<float>(std::clamp(0.5 + ty / h, hy, 1 - hy));
    roll = static_cast<float>(turn);
}
}
