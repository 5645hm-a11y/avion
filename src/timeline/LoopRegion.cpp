#include "media/timeline/LoopRegion.hpp"
namespace media::timeline {
namespace {
bool Valid(std::int64_t position, std::int64_t duration) noexcept { return duration > 0 && position >= 0 && position <= duration; }
}
LoopRegion::Step LoopRegion::Toggle(std::int64_t position, std::int64_t duration) noexcept {
    switch (state_) {
    case State::Off: return SetStart(position, duration);
    case State::Armed: return SetEnd(position, duration);
    case State::Looping: return Clear();
    }
    return Step::Unavailable;
}
LoopRegion::Step LoopRegion::SetStart(std::int64_t position, std::int64_t duration) noexcept {
    if (!Valid(position, duration)) return Step::Unavailable;
    state_ = State::Armed; a_ = position; b_ = 0; cue_ = false; wraps_ = 0;
    return Step::MarkedStart;
}
LoopRegion::Step LoopRegion::SetEnd(std::int64_t position, std::int64_t duration) noexcept {
    if (!Valid(position, duration)) return Step::Unavailable;
    if (state_ == State::Off) return Step::NoStart;
    std::int64_t a = a_, b = position;
    if (b < a) { const auto t = a; a = b; b = t; } // Marked the other way round.
    if (b - a < kMinimum) return Step::TooShort;
    state_ = State::Looping; a_ = a; b_ = b; cue_ = false; wraps_ = 0;
    return Step::Looping;
}
LoopRegion::Step LoopRegion::SetCue(std::int64_t start, std::int64_t end, std::int64_t duration) noexcept {
    if (duration <= 0 || start < 0 || end <= start || start >= duration) return Step::Unavailable;
    const std::int64_t a = start > kCuePad ? start - kCuePad : 0;
    const std::int64_t b = end + kCuePad < duration ? end + kCuePad : duration;
    if (b - a < kMinimum) return Step::TooShort;
    state_ = State::Looping; a_ = a; b_ = b; cue_ = true; wraps_ = 0;
    return Step::Looping;
}
LoopRegion::Step LoopRegion::Clear() noexcept {
    state_ = State::Off; a_ = b_ = 0; cue_ = false;
    return Step::Cleared;
}
std::uint16_t LoopRegion::Fraction(std::int64_t time, std::int64_t duration) noexcept {
    if (duration <= 0) return 0;
    const double f = static_cast<double>(time < 0 ? 0 : time > duration ? duration : time) / static_cast<double>(duration);
    return static_cast<std::uint16_t>(1 + static_cast<unsigned>(f * 65534.0 + 0.5));
}
std::uint32_t LoopRegion::Packed(std::int64_t duration) const noexcept {
    if (state_ == State::Off || duration <= 0) return 0;
    const std::uint32_t a = Fraction(a_, duration), b = state_ == State::Looping ? Fraction(b_, duration) : 0u;
    return a | b << 16;
}
}
