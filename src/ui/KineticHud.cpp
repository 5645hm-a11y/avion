#include "media/ui/KineticHud.hpp"
#include <algorithm>
#include <cmath>
namespace media::ui {
namespace {
void Spring(float& value, float& speed, float target, float omega, float dt) noexcept {
    const float x = value - target, decay = std::exp(-omega * dt), carry = speed + omega * x;
    value = target + (x + carry * dt) * decay; speed = (speed - omega * carry * dt) * decay;
}
}
float KineticHud::LayoutScale(unsigned width, unsigned height, float dpi) noexcept {
    dpi = std::isfinite(dpi) ? std::clamp(dpi, 0.5f, 8.0f) : 1.0f;
    const float fit = (std::min)(static_cast<float>(width) / (1280 * dpi), static_cast<float>(height) / (720 * dpi));
    return dpi * std::clamp(fit, 0.8f, 1.25f);
}
void KineticHud::Layout(unsigned width, unsigned height, float dpi) noexcept {
    u_.scale = LayoutScale(width, height, dpi);
    u_.dock_half_width = (std::max)(64.0f, (std::min)(280.0f, (static_cast<float>(width) - 32 * u_.scale) / (2 * u_.scale)));
    u_.center_x = static_cast<float>(width) * 0.5f; u_.center_y = static_cast<float>(height) - 78 * u_.scale;
    // The volume pill takes the gap between the transport button and the duration clock (H:MM:SS at its widest);
    // a dock too narrow to leave 84 px there has no pill.
    const float left = 40, right = u_.dock_half_width - 112;
    u_.volume_half_width = right - left >= 84 ? (std::min)((right - left) / 2, 60.0f) : 0.0f;
    u_.volume_x = (left + right) / 2;
}
void KineticHud::Resize(unsigned width, unsigned height, float dpi) noexcept {
    Layout(width, height, dpi);
    // Pointer coordinates are relative to the old layout; the next move re-reports them.
    u_.pointer_x = u_.pointer_y = -1000; if (!dragging_) hover_ = HitRegion::None;
}
void KineticHud::Configure(unsigned width, unsigned height, std::int64_t duration, bool visible, float dpi) noexcept {
    *this = KineticHud{};
    Layout(width, height, dpi);
    duration_ = (std::max)(duration, std::int64_t{0}); u_.duration_seconds = static_cast<float>(duration_ / 10000000);
    always_ = visible; u_.visibility = visible ? 1.0f : 0.0f;
}
HitRegion KineticHud::Hit(float x, float y) const noexcept {
    const float dx = (x - u_.center_x) / u_.scale, dy = (y - u_.center_y) / u_.scale;
    if (std::hypot(dx, dy + 12) <= 26) return HitRegion::Core;
    if (u_.volume_half_width > 0 && u_.volume_visibility >= 0.5f && std::abs(dx - u_.volume_x) <= u_.volume_half_width &&
        std::abs(dy - kVolumeY) <= kVolumeHalfHeight + 2) return HitRegion::Volume;
    if (duration_ && std::abs(dx) <= u_.dock_half_width - 16 && std::abs(dy - u_.timeline_y) <= 16) return HitRegion::Timeline;
    return HitRegion::None;
}
std::int64_t KineticHud::Position(float x) const noexcept {
    const double half = (u_.dock_half_width - 28) * u_.scale;
    const double fraction = std::clamp((x - u_.center_x + half) / (2 * half), 0.0, 1.0);
    return fraction >= 1 ? duration_ : static_cast<std::int64_t>(fraction * static_cast<double>(duration_));
}
float KineticHud::Level(float x) const noexcept {
    const float from = u_.volume_x - u_.volume_half_width + kVolumeTrackLeft, to = u_.volume_x + u_.volume_half_width - kVolumeTrackRight;
    return std::clamp(((x - u_.center_x) / u_.scale - from) / (to - from), 0.0f, 1.0f);
}
HudAction KineticHud::Loudness(float volume, std::int64_t time) noexcept {
    // Shown at once; the engine's next report confirms the same value.
    volume_ = volume; last_volume_ = time; volume_shown_ = true;
    return {HudCommand::Volume, 0, volume};
}
void KineticHud::SetVolume(float volume, bool muted, std::int64_t now) noexcept {
    volume = volume > 0 ? (std::min)(volume, 1.0f) : 0.0f; // Also maps NaN to 0.
    if (!volume_known_) {
        volume_known_ = true; volume_ = u_.volume = volume; muted_ = muted; u_.muted = muted ? 1.0f : 0.0f; return;
    }
    if (volume == volume_ && muted == muted_) return;
    volume_ = volume; muted_ = muted; last_volume_ = now; volume_shown_ = true;
}
void KineticHud::SetLoop(std::uint32_t packed, std::int64_t now) noexcept {
    if (packed == u_.loop) return;
    u_.loop = packed; Reveal(now); // The bracket is seen.
}
void KineticHud::Reveal(std::int64_t now) noexcept {
    if (now >= 0 && (!seen_ || now >= last_input_)) { seen_ = true; last_input_ = now; } // Shown as after an input.
}
HudAction KineticHud::Commit(std::int64_t target) noexcept {
    if (!duration_) return {};
    target_ = std::clamp(target, std::int64_t{0}, duration_); committed_ = true; age_ = 0;
    return {HudCommand::Seek, target_};
}
HudAction KineticHud::Input(const PointerEvent& e, bool paused) noexcept {
    if (!std::isfinite(e.x) || !std::isfinite(e.y) || e.time < 0 || (seen_ && e.time < last_input_)) return {};
    const bool visible = always_ || u_.visibility >= 0.5f;
    const double dt = (std::max)(0.001, static_cast<double>(e.time - last_input_) / 1e7);
    seen_ = true; last_input_ = e.time;
    const auto region = Hit(e.x, e.y);
    switch (e.kind) {
    case PointerEvent::Kind::Move:
        hover_ = region; u_.pointer_x = (e.x - u_.center_x) / u_.scale; u_.pointer_y = (e.y - u_.center_y) / u_.scale;
        if (volume_dragging_) return Loudness(Level(e.x), e.time);
        if (dragging_) {
            const auto next = Position(e.x);
            u_.scrub_velocity = duration_ ? static_cast<float>(std::clamp(static_cast<double>(next - target_) / static_cast<double>(duration_) / dt, -4.0, 4.0)) : 0;
            const bool moved = next != target_;
            target_ = next;
            if (moved) return {HudCommand::Scrub, target_}; // The engine previews while the drag moves (docs/seeking.md).
        }
        break;
    case PointerEvent::Kind::Down:
        if (!visible) break;
        u_.focus = 0; hover_ = region;
        if (region == HitRegion::Core) pressed_ = true;
        if (region == HitRegion::Timeline) { dragging_ = true; committed_ = false; target_ = Position(e.x); }
        if (region == HitRegion::Volume) { volume_dragging_ = true; return Loudness(Level(e.x), e.time); }
        break;
    case PointerEvent::Kind::Up:
        if (volume_dragging_) { volume_dragging_ = false; last_volume_ = e.time; break; }
        if (dragging_) { dragging_ = false; return Commit(Position(e.x)); }
        if (pressed_) { pressed_ = false; if (region == HitRegion::Core) return {paused ? HudCommand::Resume : HudCommand::Pause, 0}; }
        break;
    case PointerEvent::Kind::Leave: hover_ = HitRegion::None; break;
    case PointerEvent::Kind::Cancel: dragging_ = pressed_ = committed_ = volume_dragging_ = false; hover_ = HitRegion::None; u_.focus = 0; target_ = media_; break;
    case PointerEvent::Kind::FocusNext: u_.focus = u_.focus == 1 && duration_ ? 2.0f : 1.0f; break;
    case PointerEvent::Kind::Activate:
        if (u_.focus == 2) return Commit(committed_ ? target_ : media_);
        [[fallthrough]];
    case PointerEvent::Kind::TogglePause: return {paused ? HudCommand::Resume : HudCommand::Pause, 0};
    case PointerEvent::Kind::SeekBack: u_.focus = 2; return Commit((committed_ ? target_ : media_) < 50000000 ? 0 : (committed_ ? target_ : media_) - 50000000);
    case PointerEvent::Kind::SeekForward: {
        u_.focus = 2; const auto base = committed_ ? target_ : media_;
        return Commit(duration_ - base < 50000000 ? duration_ : base + 50000000);
    }
    case PointerEvent::Kind::SeekStart: u_.focus = 2; return Commit(0);
    case PointerEvent::Kind::SeekEnd: u_.focus = 2; return Commit(duration_);
    case PointerEvent::Kind::Wheel: {
        // One notch is one 5 % step on the keyboard's grid. At either end the pill still answers.
        const long notches = std::lround(e.y);
        if (!volume_known_ || !notches) break;
        return Loudness(static_cast<float>(std::clamp(std::lround(volume_ * 20) + notches, 0L, 20L)) / 20, e.time);
    }
    }
    return {};
}
HudAction KineticHud::Advance(std::int64_t now, bool paused, std::int64_t media, unsigned presented) noexcept {
    (void)presented;
    if (now < 0 || now < last_advance_) return {};
    const float dt = last_advance_ ? std::clamp(static_cast<float>(static_cast<double>(now - last_advance_) / 1e7), 0.0f, 0.1f) : 0;
    last_advance_ = now; media_ = std::clamp(media, std::int64_t{0}, duration_ ? duration_ : INT64_MAX);
    u_.time = std::fmod(u_.time + dt, 1000.0f);
    if (committed_) {
        age_ += dt; const auto distance = target_ > media_ ? target_ - media_ : media_ - target_;
        if (distance <= 2500000 || age_ > kCommitTimeoutSeconds) committed_ = false;
    }
    if (!dragging_ && !committed_) target_ = media_;
    u_.progress = duration_ ? static_cast<float>(static_cast<double>(media_) / static_cast<double>(duration_)) : 0;
    u_.scrub_offset = duration_ ? static_cast<float>(static_cast<double>(target_ - media_) / static_cast<double>(duration_)) : 0;
    u_.elapsed_seconds = static_cast<float>((dragging_ || committed_ ? target_ : media_) / 10000000);
    const float response = 1 - std::exp(-dt / 0.08f);
    u_.hover += ((hover_ != HitRegion::None ? 1.0f : 0.0f) - u_.hover) * response;
    u_.timeline_hover += ((dragging_ || hover_ == HitRegion::Timeline || u_.focus == 2 ? 1.0f : 0.0f) - u_.timeline_hover) * response;
    u_.pulse += ((pressed_ ? 1.0f : 0.0f) - u_.pulse) * response;
    u_.denied *= std::exp(-dt / 0.18f);
    if (!dragging_) u_.scrub_velocity *= std::exp(-dt / 0.12f);
    // Volume pill: shown by a change for kVolumeHideSeconds, and while the pointer holds it; a mute keeps it in the
    // dock without keeping the dock itself awake.
    const bool adjusting = volume_dragging_ || hover_ == HitRegion::Volume;
    if (volume_shown_ && now - last_volume_ >= static_cast<std::int64_t>(kVolumeHideSeconds * 1e7f)) volume_shown_ = false;
    const bool loud = adjusting || volume_shown_;
    const float pill = u_.volume_half_width > 0 && (loud || muted_) ? 1.0f : 0.0f, mute = muted_ ? 1.0f : 0.0f;
    const auto glide = [](float& value, float target, float rate) { value += (target - value) * rate; if (std::abs(target - value) < 1e-3f) value = target; };
    const bool still = u_.reduced_motion != 0;
    glide(u_.volume, volume_, still ? 1.0f : 1 - std::exp(-dt / 0.06f));
    glide(u_.muted, mute, still ? 1.0f : response);
    glide(u_.volume_visibility, pill, still ? 1.0f : 1 - std::exp(-dt / 0.10f));
    glide(u_.volume_hover, adjusting ? 1.0f : 0.0f, still ? 1.0f : response);
    glide(u_.preview, preview_ ? 1.0f : 0.0f, still ? 1.0f : response);
    const bool wake = always_ || paused || dragging_ || committed_ || loud || hover_ != HitRegion::None || u_.focus != 0 || (seen_ && now - last_input_ < 25000000);
    if (u_.reduced_motion != 0) { u_.energy = paused ? 0.0f : 1.0f; u_.visibility = wake ? 1.0f : 0.0f; u_.scrub_velocity = 0; }
    else { Spring(u_.energy, energy_speed_, paused ? 0.0f : 1.0f, 24, dt); Spring(u_.visibility, visibility_speed_, wake ? 1.0f : 0.0f, 24, dt); }
    u_.energy = std::clamp(u_.energy, 0.0f, 1.0f); u_.freeze = 1 - u_.energy;
    u_.visibility = std::clamp(u_.visibility, 0.0f, 1.0f); return {};
}
bool TimelineMarks::Add(double fraction, bool chapter) noexcept {
    if (count >= kMax || !(fraction >= 0 && fraction <= 1)) return false; // Also refuses NaN.
    const auto position = static_cast<std::uint32_t>(1 + std::lround(fraction * 32766.0)) | (chapter ? 0x8000u : 0u);
    packed[count / 2] |= position << (16 * (count % 2)); ++count;
    return true;
}
double TimelineMarks::Fraction(unsigned i) const noexcept {
    if (i >= count) return 0;
    return static_cast<double>(((packed[i / 2] >> (16 * (i % 2))) & 0x7fffu) - 1) / 32766.0;
}
bool TimelineMarks::Chapter(unsigned i) const noexcept { return i < count && ((packed[i / 2] >> (16 * (i % 2))) & 0x8000u) != 0; }
void KineticHud::SeekRejected() noexcept { committed_ = dragging_ = false; target_ = media_; u_.denied = 1; }
}
