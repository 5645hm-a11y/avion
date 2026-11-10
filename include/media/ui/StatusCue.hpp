#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
namespace media::ui {
enum class StatusIcon : std::uint8_t { None, Audio, Subtitles, Voice, Device, Warning };
// Warning: an amber rim and icon (a refusal, a missing track). Listening: a cool rim and halo that breathe slowly (the
// wake word heard alone: the player waits for a command, silently).
enum class StatusTone : std::uint8_t { Info, Warning, Listening };
// 16 root constants. Positions are client pixels; geometry is in design pixels.
struct StatusUniforms {
    float center_x = 0, center_y = 0, scale = 1, visibility = 0;
    float half_width = 60, half_height = 19, icon = 0, tone = 0;
    float travel = 0, font = 15, pulse = 0, reserved1 = 0; // travel: design px above its rest while fading; pulse: Listening's breath, 0-1.
    float reserved2 = 0, reserved3 = 0, reserved4 = 0, reserved5 = 0;
};
static_assert(sizeof(StatusUniforms) == 64);
static_assert(std::is_standard_layout_v<StatusUniforms> && std::is_trivially_copyable_v<StatusUniforms>);
// The lane the text is rasterised into, client pixels: after the icon, as wide as the widest pill allows.
struct StatusTextRect { int x = 0, y = 0; unsigned width = 0, height = 0; float font_pixels = 0; };

// A transient system cue ("Audio · Hebrew (עברית) · AAC stereo", "Subtitles off", "No French audio"): a glass pill at
// the top centre of the window. Portable and allocation-free; it draws nothing (render::StatusPipeline does).
// - Show() replaces the text. The pill appears only once that text's raster exists (Ready(), from the renderer): a
//   cue is seen whole or not yet, never as an empty pill.
// - Its motion is a closed-form function of the time since Ready: 0.22 s in (ease-out cubic, sliding 8 px down),
//   2.5 s held (or as long as Show asks: the listening cue stays for the wake window), 0.5 s out (ease-in cubic). A
//   new text while the pill is up keeps it up, restarts the hold, and the width glides to the new text's over 0.15 s.
//   Reduced motion: no slide, 0.1 s fades, and a Listening pill glows steadily instead of breathing.
// - A line feed starts a new line (at most kMaxLines; any further one is a space): the pill becomes a glass panel, one
//   kLineHeight taller per line, as wide as kMaxPanelHalfWidth allows (the telemetry readout, docs/kinetic-hud.md). Its top
//   edge stays where the pill's is. The panel takes its new height when its new text appears, never before.
class StatusCue final {
public:
    static constexpr unsigned kMaxBytes = 384, kMaxLines = 8;
    static constexpr float kInSeconds = 0.22f, kHoldSeconds = 2.5f, kOutSeconds = 0.5f, kWidthSeconds = 0.15f, kReducedSeconds = 0.1f;
    static constexpr float kPulseSeconds = 1.6f, kMinHold = 0.5f, kMaxHold = 30;
    static constexpr float kHalfHeight = 19, kMinHalfWidth = 56, kMaxHalfWidth = 300, kIconWidth = 30, kPadding = 16;
    static constexpr float kFontPixels = 15, kTop = 26, kTravel = 8, kLineHeight = 20, kMaxPanelHalfWidth = 420;
    // Centred horizontally, kTop scaled pixels below the top edge; the scale is the dock's (KineticHud::LayoutScale).
    void Layout(unsigned width, unsigned height, float dpi_scale) noexcept;
    void SetReducedMotion(bool reduced) noexcept { reduced_ = reduced; }
    // The text is copied (at most kMaxBytes, cut before an incomplete character; control characters become spaces and
    // ill-formed UTF-8 a question mark). Empty text hides the pill.
    // `hold_seconds`: how long this text stays up once shown whole (kMinHold to kMaxHold).
    void Show(std::string_view utf8, StatusIcon, StatusTone, std::int64_t now, float hold_seconds = kHoldSeconds) noexcept;
    float Hold() const noexcept { return shown_hold_; } // The hold of the text on screen.
    // The raster of `revision` is ready to be drawn; `ink` is the text's width in client pixels.
    void Ready(std::uint32_t revision, float ink, std::int64_t now) noexcept;
    void Advance(std::int64_t now) noexcept;
    std::string_view Text() const noexcept { return {text_, size_}; }
    std::uint32_t Revision() const noexcept { return revision_; }  // Changes with every Show.
    // A warm session's next file (docs/session.md): revisions go on from the previous file's cue, so a raster made for
    // one of its texts is never taken for a text of this one. Before the first Show.
    void ContinueRevision(std::uint32_t revision) noexcept { if (!size_ && revision > revision_) revision_ = revision; }
    std::uint32_t ShownRevision() const noexcept { return shown_; } // The revision the pill shows (0: none yet).
    StatusIcon Icon() const noexcept { return icon_; }
    StatusTone Tone() const noexcept { return tone_; }
    const StatusUniforms& Uniforms() const noexcept { return u_; }
    StatusTextRect TextRect() const noexcept;    // The lane of the text on screen (its lines).
    StatusTextRect PendingRect() const noexcept; // The lane the text to show next is rasterised for (its lines).
    unsigned Lines() const noexcept { return lines_; }             // Of the text to show next.
    unsigned ShownLines() const noexcept { return shown_lines_; }  // Of the text on screen.
    // The text lane's left edge follows the pill's (its width follows the text): where the raster's first ink column goes.
    float TextLeft() const noexcept;
    bool Visible() const noexcept { return u_.visibility > 0.002f; }
    // Something to draw or about to be: up, fading, or waiting for its raster.
    bool Animating() const noexcept { return up_ || Visible() || (size_ && revision_ != shown_); }
    // In or out (not held): the renderer refreshes at the display's rate then, also while paused.
    bool Fading() const noexcept { return up_ && u_.visibility < 0.999f; }
private:
    float Target(float ink) const noexcept;
    float HalfHeight(unsigned lines) const noexcept { return kHalfHeight + 0.5f * kLineHeight * static_cast<float>((lines ? lines : 1) - 1); }
    StatusTextRect Rect(unsigned lines) const noexcept;
    void Place() noexcept; // Height and centre of the panel for the lines on screen.
    StatusUniforms u_{};
    char text_[kMaxBytes]{};
    unsigned size_ = 0, lines_ = 1, shown_lines_ = 1, width_ = 0;
    std::uint32_t revision_ = 0, shown_ = 0;
    std::int64_t ready_at_ = 0, width_at_ = 0;
    float width_from_ = kMinHalfWidth, width_to_ = kMinHalfWidth, lane_ = 0, dpi_ = 1;
    float hold_ = kHoldSeconds, shown_hold_ = kHoldSeconds;
    StatusIcon icon_ = StatusIcon::None, shown_icon_ = StatusIcon::None;
    StatusTone tone_ = StatusTone::Info, shown_tone_ = StatusTone::Info;
    bool reduced_ = false, up_ = false;
};
// "Audio · Commentary · Hebrew (עברית) · AAC stereo": a track's cue text. `english`/`native`: the language's names
// (nullptr when unknown; `code` is shown then, else "Track <id>"); `channels`: 0 for a subtitle track. Returns bytes.
std::size_t TrackLabel(char* out, std::size_t capacity, bool audio, const char* name, const char* english, const char* native,
                       const char* code, unsigned channels, std::uint32_t id) noexcept;
// A subtitle cue's text as a quote in a cue, with its quotation marks ("זהירות! שלושת הסנאים…"), as it is displayed (tags dropped, entities
// decoded, line breaks as spaces: text::Normalize with kMarkup), at most `letters` characters; a longer text is cut at the
// last space among the final quarter of them (else at `letters`) and ends with an ellipsis. Returns the bytes written,
// NUL-terminated; 0 when the text is empty or not UTF-8, or the quote does not fit.
std::size_t QuoteExcerpt(std::string_view cue_text, char* out, std::size_t capacity, unsigned letters) noexcept;
}
