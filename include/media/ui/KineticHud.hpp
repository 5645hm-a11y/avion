#pragma once
#include <cstdint>
#include <type_traits>
namespace media::ui {
struct PointerEvent {
    // Wheel: `y` is the number of notches, positive away from the user (louder).
    enum class Kind : std::uint8_t { Move, Down, Up, Leave, TogglePause, Cancel, FocusNext, Activate, SeekBack, SeekForward, SeekStart, SeekEnd, Wheel } kind = Kind::Move;
    float x = 0, y = 0;
    std::int64_t time = 0;
};
// 32 root constants. Positions are client pixels; geometry below is in design pixels.
struct HudUniforms {
    float center_x = 0, center_y = 0, scale = 1, time = 0;
    float energy = 1, freeze = 0, pulse = 0, visibility = 0;
    float progress = 0, scrub_offset = 0, scrub_velocity = 0, hover = 0;
    float denied = 0, core_radius = 22, dock_half_width = 280, dock_half_height = 52;
    float pointer_x = -1000, pointer_y = -1000, timeline_hover = 0, timeline_y = 28;
    float elapsed_seconds = 0, duration_seconds = 0, focus = 0, reduced_motion = 0;
    // Volume pill, between the transport button and the duration clock: the animated fill level, its fade, the
    // animated mute cue and pointer emphasis; then its centre and half-width (0: the dock is too narrow for it).
    float volume = 1, volume_visibility = 0, muted = 0, volume_hover = 0;
    // Row 8 also carries the scrub preview cue (0 to 1, animated): the picture on screen is a keyframe preview, not the
    // exact frame at the knob (docs/seeking.md, "Scrub preview"), and the A–B loop's bracket (timeline::LoopRegion::Packed:
    // A in the low 16 bits, B in the high, positions along the track; 0: no loop).
    float volume_x = 0, volume_half_width = 0, preview = 0;
    std::uint32_t loop = 0;
};
static_assert(sizeof(HudUniforms) == 128);
static_assert(std::is_standard_layout_v<HudUniforms> && std::is_trivially_copyable_v<HudUniforms>);
// Chapter and scene ticks on the timeline (docs/kinetic-hud.md, "Timeline marks"): 16 more root constants, two marks a
// constant. A mark is 16 bits: bit 15 set for a chapter (else a scene), bits 0-14 its position along the track, 1 (the
// start) to 32767 (the end); 0 is no mark. At most kMax are drawn; Add refuses the rest.
struct TimelineMarks {
    static constexpr unsigned kMax = 32;
    std::uint32_t packed[kMax / 2]{};
    unsigned count = 0;
    bool Add(double fraction, bool chapter) noexcept;
    void Clear() noexcept { *this = TimelineMarks{}; }
    // The mark at `i` (in the order added): its fraction (0 to 1) and kind.
    double Fraction(unsigned i) const noexcept;
    bool Chapter(unsigned i) const noexcept;
};
static_assert(sizeof(TimelineMarks::packed) == 64);
// The file's bookmarks on the timeline (docs/session.md, "Bookmarks"): 16 more root constants, two a constant, each a 16-bit
// position along the track (1 the start to 65535 the end; 0 none), drawn as small amber flags above it.
struct BookmarkMarks {
    static constexpr unsigned kMax = 32;
    std::uint32_t packed[kMax / 2]{};
    void Set(unsigned index, std::uint16_t position) noexcept {
        if (index >= kMax) return;
        auto& word = packed[index / 2]; const unsigned shift = 16 * (index % 2);
        word = (word & ~(0xffffu << shift)) | (static_cast<std::uint32_t>(position) << shift);
    }
    void Clear() noexcept { for (auto& w : packed) w = 0; }
};
static_assert(sizeof(BookmarkMarks::packed) == 64);
enum class HitRegion : std::uint8_t { None, Core, Timeline, Volume };
// Scrub: the timeline is being dragged (the target moved); Seek: released, or a click, a key. A drag emits Scrub on every
// move that changes the target, then Seek on release. Chapter: Page Down (seek_target 1) or Page Up (-1).
// Chapter: seek_target +1 / -1 (Page Down / Page Up). File: the next (+1) or previous (-1) file in the folder (Ctrl+Right /
// Ctrl+Left; docs/session.md). Speed: one step faster (+1) or slower (-1) (] and [; docs/audio-dsp.md). Step: one picture
// forward (+1) or back (-1), pausing first (. and ,). Loop: the L key (the A–B loop, docs/seeking.md). Stats: the I key (the
// telemetry panel, docs/session.md). Night: the K key (night mode). Bookmark: B (seek_target 0: a bookmark, which also pins
// the frame) or Ctrl+B (1: the frame pinned alone).
enum class HudCommand : std::uint8_t { None, Pause, Resume, Seek, Volume, Scrub, Chapter, File, Speed, Step, Loop, Stats, Night, Bookmark, Track, NextTarget,
                                       Snapshot };
struct HudAction { HudCommand command = HudCommand::None; std::int64_t seek_target = 0; float volume = 0; };
class KineticHud final {
public:
    static constexpr float kIdleHideSeconds = 2.5f, kArrivalSeconds = 0.25f, kCommitTimeoutSeconds = 3;
    // The volume pill fades this long after the last change (it stays while hovered, dragged or muted).
    static constexpr float kVolumeHideSeconds = 1.5f;
    // Pill geometry, design pixels: the row it shares with the transport button, its half-height, and the insets
    // of the level track from the pill's left (after the speaker glyph) and right ends.
    static constexpr float kVolumeY = -12, kVolumeHalfHeight = 12, kVolumeTrackLeft = 34, kVolumeTrackRight = 12;
    // `dpi_scale`: display DPI / 96. Resets all interaction state.
    void Configure(unsigned width, unsigned height, std::int64_t duration, bool always_visible, float dpi_scale = 1) noexcept;
    // New client size or DPI: re-lays out the dock and keeps playback/interaction state (a drag keeps its target).
    // Scale = dpi * clamp(min(width / 1280, height / 720) in device-independent pixels, 0.8, 1.25); the dock stays
    // 26 scaled pixels above the bottom edge and narrows (never below 64 design px) to keep 16 scaled px of side margin.
    void Resize(unsigned width, unsigned height, float dpi_scale) noexcept;
    static float LayoutScale(unsigned width, unsigned height, float dpi_scale) noexcept;
    void SetReducedMotion(bool reduced) noexcept { u_.reduced_motion = reduced ? 1.0f : 0.0f; }
    // The engine's output volume (0 to 1) and mute, reported as often as convenient. The first report is the
    // baseline; a later one that differs reveals the dock and the pill, whatever changed it (keys, wheel, a command).
    void SetVolume(float volume, bool muted, std::int64_t now) noexcept;
    // The picture on screen is a keyframe preview (true) or exact: the knob turns into a ring and a diamond marks where
    // the previewed picture really is, until an exact landing (Advance glides `preview` over 80 ms).
    void SetPreview(bool preview) noexcept { preview_ = preview; }
    // The A–B loop's bracket on the timeline (HudUniforms::loop). A change reveals the dock, so the bracket is seen.
    void SetLoop(std::uint32_t packed, std::int64_t now) noexcept;
    void Reveal(std::int64_t now) noexcept; // Shows the dock as an input would (a new bookmark, for one).
    bool Preview() const noexcept { return preview_; }
    HitRegion Hit(float x, float y) const noexcept;
    HudAction Input(const PointerEvent&, bool paused) noexcept;
    HudAction Advance(std::int64_t now, bool paused, std::int64_t media, unsigned presented) noexcept;
    void SeekRejected() noexcept;
    const HudUniforms& Uniforms() const noexcept { return u_; }
    bool Animating() const noexcept { return u_.visibility > 0.002f; }
    // Nothing that ends with time (a drag, a seek's commit, the volume pill's 1.5 s, a press's pulse): with its uniforms
    // drawn, an advance would change nothing seen (the paused presentation thread then sleeps; D3D12Renderer::SetSettledWait).
    bool Settled() const noexcept { return !dragging_ && !committed_ && !volume_shown_ && !pressed_ && !volume_dragging_; }
    bool Dragging() const noexcept { return dragging_; }
    bool Committed() const noexcept { return committed_; }
    double ScrubSeconds() const noexcept { return static_cast<double>(target_ - media_) / 1e7; }
private:
    std::int64_t Position(float x) const noexcept;
    float Level(float x) const noexcept;
    void Layout(unsigned width, unsigned height, float dpi_scale) noexcept;
    HudAction Commit(std::int64_t target) noexcept;
    HudAction Loudness(float volume, std::int64_t time) noexcept;
    HudUniforms u_{};
    std::int64_t duration_ = 0, media_ = 0, target_ = 0, last_input_ = 0, last_advance_ = 0, last_volume_ = 0;
    float energy_speed_ = 0, visibility_speed_ = 0, age_ = 0, volume_ = 1;
    bool always_ = false, dragging_ = false, pressed_ = false, committed_ = false, seen_ = false;
    bool volume_known_ = false, volume_shown_ = false, muted_ = false, volume_dragging_ = false, preview_ = false;
    HitRegion hover_ = HitRegion::None;
};
}
