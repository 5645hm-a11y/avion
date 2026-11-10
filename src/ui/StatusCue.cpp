#include "media/ui/StatusCue.hpp"
#include "media/ui/KineticHud.hpp"
#include "media/text/Utf8.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace media::ui {
void StatusCue::Layout(unsigned width, unsigned height, float dpi) noexcept {
    dpi_ = dpi; width_ = width;
    u_.scale = KineticHud::LayoutScale(width, height, dpi);
    u_.font = kFontPixels;
    u_.center_x = std::floor(static_cast<float>(width) * 0.5f);
    lane_ = (2 * kMaxHalfWidth - kIconWidth - 2 * kPadding) * u_.scale;
    Place();
    // The pill keeps its width in design pixels: nothing to redo for a new scale except the lane.
}
void StatusCue::Place() noexcept {
    u_.half_height = HalfHeight(shown_lines_);
    u_.center_y = std::floor((kTop + u_.half_height) * u_.scale);
}
StatusTextRect StatusCue::Rect(unsigned lines) const noexcept {
    StatusTextRect r;
    r.x = static_cast<int>(std::round(TextLeft()));
    const float half = HalfHeight(lines), centre = std::floor((kTop + half) * u_.scale);
    const float top = std::round(centre - half * u_.scale), bottom = std::round(centre + half * u_.scale);
    r.y = static_cast<int>(top);
    // A panel's lane is as wide as the widest panel, inside the window.
    float lane = lane_;
    if (lines > 1 && u_.scale > 0) {
        const float widest = (std::min)(kMaxPanelHalfWidth, (std::max)(kMaxHalfWidth, static_cast<float>(width_) / u_.scale * 0.5f - 12));
        lane = (2 * widest - kIconWidth - 2 * kPadding) * u_.scale;
    }
    r.width = lane > 0 ? static_cast<unsigned>(std::round(lane)) : 0; r.height = bottom > top ? static_cast<unsigned>(bottom - top) : 0;
    r.font_pixels = u_.font * u_.scale;
    return r;
}
StatusTextRect StatusCue::TextRect() const noexcept { return Rect(shown_lines_); }
StatusTextRect StatusCue::PendingRect() const noexcept { return Rect(lines_); }
float StatusCue::TextLeft() const noexcept { return u_.center_x - u_.half_width * u_.scale + (kPadding + kIconWidth) * u_.scale; }
float StatusCue::Target(float ink) const noexcept {
    const float design = ink / (u_.scale > 0 ? u_.scale : 1);
    const float widest = shown_lines_ > 1 && u_.scale > 0 ?
        (std::min)(kMaxPanelHalfWidth, (std::max)(kMaxHalfWidth, static_cast<float>(width_) / u_.scale * 0.5f - 12)) : kMaxHalfWidth;
    return std::clamp((2 * kPadding + kIconWidth + design) * 0.5f, kMinHalfWidth, widest);
}
void StatusCue::Show(std::string_view utf8, StatusIcon icon, StatusTone tone, std::int64_t now, float hold_seconds) noexcept {
    size_ = 0; lines_ = 1;
    hold_ = std::clamp(hold_seconds == hold_seconds ? hold_seconds : kHoldSeconds, kMinHold, kMaxHold);
    while (!utf8.empty()) {
        std::uint32_t scalar = 0; auto rest = utf8;
        if (!text::Utf8(rest, scalar)) { rest = utf8.substr(1); scalar = '?'; }
        // A line feed between two lines starts the next one (kMaxLines at most); every other control is a space.
        if (scalar == '\n' && (!size_ || text_[size_ - 1] == '\n')) { utf8 = rest; continue; } // No empty line.
        const bool line = scalar == '\n' && lines_ < kMaxLines;
        if (!line && (scalar < 0x20 || scalar == 0x7f || (scalar >= 0x80 && scalar <= 0x9f))) scalar = ' ';
        char bytes[4]; const auto n = text::Encode(scalar, bytes);
        if (size_ + n > kMaxBytes) break; // Cut before a character that does not fit, never inside one.
        std::memcpy(text_ + size_, bytes, n); size_ += n; utf8 = rest;
        if (line) ++lines_;
    }
    while (size_ && text_[size_ - 1] == '\n') { --size_; --lines_; } // No empty last line.
    icon_ = icon; tone_ = tone; ++revision_;
    if (!size_ && up_) {
        // Nothing to say: the pill leaves now.
        const auto in = static_cast<std::int64_t>((reduced_ ? kReducedSeconds : kInSeconds) * 1e7f);
        ready_at_ = (std::min)(ready_at_, now - in - static_cast<std::int64_t>(shown_hold_ * 1e7f));
    }
}
void StatusCue::Ready(std::uint32_t revision, float ink, std::int64_t now) noexcept {
    if (revision != revision_ || !size_) return;
    Advance(now); // Whether the pill is still up is a matter of `now`, not of the last frame drawn.
    shown_lines_ = lines_; Place(); // The panel takes the new text's height with it.
    const float target = Target(ink);
    const auto in = static_cast<std::int64_t>((reduced_ ? kReducedSeconds : kInSeconds) * 1e7f);
    if (up_ && Visible()) {
        // Already up: it stays up with the new text, the hold starts again, the width glides.
        width_from_ = u_.half_width; width_to_ = target; width_at_ = now;
        ready_at_ = now - (u_.visibility >= 0.999f ? in : static_cast<std::int64_t>(static_cast<float>(in) * u_.visibility));
    } else { width_from_ = width_to_ = target; width_at_ = now; ready_at_ = now; }
    shown_ = revision; shown_icon_ = icon_; shown_tone_ = tone_; shown_hold_ = hold_; up_ = true;
}
void StatusCue::Advance(std::int64_t now) noexcept {
    u_.icon = static_cast<float>(shown_icon_); u_.tone = static_cast<float>(shown_tone_);
    if (!up_) { u_.visibility = 0; u_.travel = reduced_ ? 0.0f : kTravel; return; }
    const float in = reduced_ ? kReducedSeconds : kInSeconds, out = reduced_ ? kReducedSeconds : kOutSeconds;
    const float s = static_cast<float>(static_cast<double>(now - ready_at_) / 1e7);
    const float hold = shown_hold_;
    float v = 0;
    if (s < 0) v = 0;
    else if (s < in) { const float x = 1 - s / in; v = 1 - x * x * x; }                 // Ease-out cubic.
    else if (s < in + hold) v = 1;
    else if (s < in + hold + out) { const float x = (s - in - hold) / out; v = 1 - x * x * x; } // Ease-in cubic.
    else { v = 0; up_ = false; }
    // Listening breathes: a raised cosine from 0 to 1 and back every kPulseSeconds, from the moment it is shown.
    if (shown_tone_ == StatusTone::Listening) u_.pulse = reduced_ ? 0.6f : 0.5f - 0.5f * std::cos(6.2831853f * (s > 0 ? s : 0.0f) / kPulseSeconds);
    else u_.pulse = 0;
    u_.visibility = std::clamp(v, 0.0f, 1.0f);
    u_.travel = reduced_ ? 0.0f : (1 - u_.visibility) * kTravel;
    const float w = std::clamp(static_cast<float>(static_cast<double>(now - width_at_) / 1e7) / kWidthSeconds, 0.0f, 1.0f);
    const float eased = reduced_ ? 1.0f : 1 - (1 - w) * (1 - w) * (1 - w);
    u_.half_width = width_from_ + (width_to_ - width_from_) * eased;
}
std::size_t TrackLabel(char* out, std::size_t capacity, bool audio, const char* name, const char* english, const char* native,
                       const char* code, unsigned channels, std::uint32_t id) noexcept {
    if (!out || !capacity) return 0;
    std::size_t n = 0; out[0] = 0;
    // Pieces go in whole or not at all, a separator only with what follows it: the text stays well formed and readable.
    const auto add = [&](const char* a, const char* b = "", const char* c = "") {
        const auto la = std::strlen(a), lb = std::strlen(b), lc = std::strlen(c);
        if (n + la + lb + lc + 1 > capacity) return false;
        std::memcpy(out + n, a, la); std::memcpy(out + n + la, b, lb); std::memcpy(out + n + la + lb, c, lc);
        n += la + lb + lc; out[n] = 0; return true;
    };
    const char* dot = " \xc2\xb7 ";
    add(audio ? "Audio" : "Subtitles");
    if (name && *name) add(dot, name);
    if (english) {
        if (add(dot, english) && native && std::strcmp(native, english) != 0) add(" (", native, ")");
    } else if (code && *code) add(dot, code);
    else if (!name || !*name) { char track[24]; std::snprintf(track, sizeof(track), "Track %u", id); add(dot, track); }
    if (audio) add(dot, channels == 6 ? "AAC 5.1" : channels == 1 ? "AAC mono" : "AAC stereo");
    return n;
}
std::size_t QuoteExcerpt(std::string_view cue_text, char* out, std::size_t capacity, unsigned letters) noexcept {
    if (!out || !capacity) return 0;
    out[0] = 0;
    char shown[4096];
    const auto bytes = text::Normalize(cue_text, shown, sizeof(shown), text::kMarkup);
    if (bytes == text::kInvalid || !letters) return 0;
    std::string_view rest{shown, bytes};
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    while (!rest.empty() && rest.back() == ' ') rest.remove_suffix(1);
    if (rest.empty()) return 0;
    // The end of the first `letters` characters, and the last space among the final quarter of them.
    std::size_t end = 0, space = 0; unsigned count = 0;
    for (std::string_view scan = rest; !scan.empty() && count < letters; ++count) {
        std::uint32_t scalar = 0;
        if (!text::Utf8(scan, scalar)) return 0;
        const auto at = rest.size() - scan.size();
        if (scalar == ' ' && count >= letters - letters / 4) space = at - 1;
        end = at;
    }
    const bool cut = end < rest.size();
    if (cut && space) end = space;
    while (cut && end && rest[end - 1] == ' ') --end;
    const std::string_view quote = rest.substr(0, end), ellipsis = cut ? "\xe2\x80\xa6" : "";
    if (2 + quote.size() + ellipsis.size() + 1 > capacity) return 0;
    std::size_t n = 0;
    out[n++] = '"'; std::memcpy(out + n, quote.data(), quote.size()); n += quote.size();
    std::memcpy(out + n, ellipsis.data(), ellipsis.size()); n += ellipsis.size(); out[n++] = '"'; out[n] = 0;
    return n;
}
}
