#include "media/ui/CommandPalette.hpp"
#include "media/ui/KineticHud.hpp"
#include "media/text/Utf8.hpp"
#include <algorithm>
#include <cmath>
namespace media::ui {
namespace {
bool Typable(std::uint32_t s) noexcept {
    return s >= 0x20 && s != 0x7f && !(s >= 0x80 && s <= 0x9f) && !(s >= 0xd800 && s <= 0xdfff) && s <= 0x10ffff && (s & 0xfffe) != 0xfffe;
}
// Pairs surrogate halves; returns the scalar, or 0 while waiting for a low half or for an unpaired one.
std::uint32_t Pair(std::uint16_t& pending, std::uint16_t unit) noexcept {
    if (unit >= 0xd800 && unit <= 0xdbff) { pending = unit; return 0; }
    if (unit >= 0xdc00 && unit <= 0xdfff) {
        const auto high = pending; pending = 0;
        return high ? 0x10000u + ((static_cast<std::uint32_t>(high) - 0xd800u) << 10) + (unit - 0xdc00u) : 0;
    }
    pending = 0; return unit;
}
}
void CommandPalette::Configure(std::span<char> storage) noexcept {
    if (storage.size() < kStorageBytes) return;
    text_ = storage.data(); composition_ = storage.data() + kMaxTextBytes;
    text_size_ = composition_size_ = 0;
}
void CommandPalette::Layout(unsigned width, unsigned height, float dpi) noexcept {
    u_.scale = KineticHud::LayoutScale(width, height, dpi);
    u_.half_width = std::clamp((static_cast<float>(width) - 48 * u_.scale) / (2 * u_.scale), 120.0f, kMaxHalfWidth);
    u_.half_height = kHalfHeight; u_.font = kFontPixels;
    u_.center_x = std::floor(static_cast<float>(width) * 0.5f);
    u_.center_y = std::floor((std::max)(static_cast<float>(height) * 0.38f, (kHalfHeight + 16) * u_.scale));
    u_.text_left = -u_.half_width + kIconWidth; u_.text_right = u_.half_width - kCounterWidth;
}
PaletteTextRect CommandPalette::TextRect() const noexcept {
    PaletteTextRect r;
    const float left = std::round(u_.center_x + u_.text_left * u_.scale), right = std::round(u_.center_x + u_.text_right * u_.scale);
    const float top = std::round(u_.center_y - u_.half_height * u_.scale), bottom = std::round(u_.center_y + u_.half_height * u_.scale);
    r.x = static_cast<int>(left); r.y = static_cast<int>(top);
    r.width = right > left ? static_cast<unsigned>(right - left) : 0; r.height = bottom > top ? static_cast<unsigned>(bottom - top) : 0;
    r.font_pixels = u_.font * u_.scale;
    return r;
}
bool CommandPalette::Open(std::int64_t now) noexcept {
    if (!text_) return false;
    open_ = true; text_size_ = composition_size_ = 0; pending_ = compose_pending_ = 0;
    hint_ = PaletteHint::Empty; u_.hint = u_.matches = 0; u_.denied = u_.shake = 0;
    last_input_ = now; ++revision_;
    return true;
}
void CommandPalette::Close(std::int64_t now) noexcept {
    if (!open_) return;
    open_ = false; pending_ = compose_pending_ = 0; last_input_ = now;
    if (composition_size_) { composition_size_ = 0; ++revision_; }
}
bool CommandPalette::Utf16(std::uint16_t unit, std::int64_t now) noexcept {
    const auto scalar = Pair(pending_, unit);
    return scalar && Scalar(scalar, now);
}
bool CommandPalette::Scalar(std::uint32_t scalar, std::int64_t now) noexcept {
    if (!open_ || !Typable(scalar) || Full()) return false;
    text_size_ += text::Encode(scalar, text_ + text_size_);
    last_input_ = now; ++revision_;
    return true;
}
void CommandPalette::BeginComposition(std::int64_t now) noexcept {
    if (!open_) return;
    composition_size_ = 0; compose_pending_ = 0; last_input_ = now; ++revision_;
}
void CommandPalette::ComposeUnit(std::uint16_t unit) noexcept {
    const auto scalar = Pair(compose_pending_, unit);
    if (!open_ || !scalar || !Typable(scalar) || composition_size_ + 4 > kMaxCompositionBytes) return;
    composition_size_ += text::Encode(scalar, composition_ + composition_size_);
}
bool CommandPalette::Backspace(std::int64_t now) noexcept {
    if (!open_ || composition_size_ || !text_size_) return false;
    do --text_size_; while (text_size_ && (static_cast<unsigned char>(text_[text_size_]) & 0xc0) == 0x80);
    pending_ = 0; last_input_ = now; ++revision_;
    return true;
}
bool CommandPalette::DeleteWord(std::int64_t now) noexcept {
    if (!open_ || composition_size_ || !text_size_) return false;
    while (text_size_ && text_[text_size_ - 1] == ' ') --text_size_;
    while (text_size_ && text_[text_size_ - 1] != ' ') --text_size_;
    pending_ = 0; last_input_ = now; ++revision_;
    return true;
}
void CommandPalette::SetHint(PaletteHint hint, unsigned matches) noexcept {
    hint_ = hint; u_.hint = static_cast<float>(hint); u_.matches = static_cast<float>((std::min)(matches, 999u));
}
void CommandPalette::Rejected(std::int64_t now) noexcept { u_.denied = 1; rejected_at_ = last_input_ = now; }
void CommandPalette::Advance(std::int64_t now) noexcept {
    if (now < 0 || now < last_advance_) return;
    const float dt = last_advance_ ? std::clamp(static_cast<float>(static_cast<double>(now - last_advance_) / 1e7), 0.0f, 0.1f) : 0;
    last_advance_ = now;
    const float target = open_ ? 1.0f : 0.0f;
    if (reduced_) { u_.visibility = target; visibility_speed_ = 0; u_.denied = u_.denied > 0.5f && now - rejected_at_ < 5000000 ? 1.0f : 0.0f; u_.shake = 0; }
    else {
        // Critically damped: no overshoot, nine tenths of the way in under 0.1 s, settled within a quarter of a second.
        const float omega = 45, x = u_.visibility - target, decay = std::exp(-omega * dt), carry = visibility_speed_ + omega * x;
        u_.visibility = target + (x + carry * dt) * decay; visibility_speed_ = (visibility_speed_ - omega * carry * dt) * decay;
        if (std::abs(u_.visibility - target) < 1e-3f && std::abs(visibility_speed_) < 5e-2f) { u_.visibility = target; visibility_speed_ = 0; }
        u_.denied *= std::exp(-dt / 0.25f);
        if (u_.denied < 1e-3f) u_.denied = 0;
        // A refusal shakes the bar sideways, three times, as it dies away.
        const float since = static_cast<float>(static_cast<double>(now - rejected_at_) / 1e7);
        u_.shake = u_.denied > 0 ? 7 * u_.denied * std::sin(since * 6.2831853f * 7) : 0.0f;
    }
    u_.visibility = std::clamp(u_.visibility, 0.0f, 1.0f);
    // The caret is steady while typing and blinks once the keys rest.
    const auto idle = now - last_input_, period = static_cast<std::int64_t>(kCaretBlinkSeconds * 1e7f);
    u_.caret = open_ && (idle < period || (idle / period) % 2 == 0) ? 1.0f : 0.0f;
}
bool CommandPalette::Hit(float x, float y) const noexcept {
    return std::abs(x - u_.center_x) <= u_.half_width * u_.scale && std::abs(y - u_.center_y) <= u_.half_height * u_.scale;
}
}
