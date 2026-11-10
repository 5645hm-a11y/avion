#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
namespace media::ui {
// What Enter would do with the typed text.
enum class PaletteHint : std::uint8_t { Empty, Command, Search, NoMatch };
// 16 root constants. Positions are client pixels; geometry is in design pixels.
struct PaletteUniforms {
    float center_x = 0, center_y = 0, scale = 1, visibility = 0;
    float half_width = 320, half_height = 28, denied = 0, caret = 0;
    // hint: a PaletteHint; matches: cues a search would find; the text lane's ends, from the bar's centre.
    float hint = 0, matches = 0, text_left = 0, text_right = 0;
    float shake = 0, font = 22, reserved0 = 0, reserved1 = 0; // shake: horizontal offset of a refusal, design px.
};
static_assert(sizeof(PaletteUniforms) == 64);
static_assert(std::is_standard_layout_v<PaletteUniforms> && std::is_trivially_copyable_v<PaletteUniforms>);
// The text lane in client pixels, where the typed text is rasterised one to one.
struct PaletteTextRect { int x = 0, y = 0; unsigned width = 0, height = 0; float font_pixels = 0; };

// The floating command bar's model: the typed text, an input method's composition, and the bar's motion. Portable
// and allocation-free: the text lives as UTF-8 in storage handed over once (the engine's arena), and every input is
// a UTF-16 code unit or a scalar, whatever the script. It draws nothing and executes nothing.
class CommandPalette final {
public:
    static constexpr unsigned kMaxTextBytes = 512, kMaxCompositionBytes = 128;
    static constexpr std::size_t kStorageBytes = kMaxTextBytes + kMaxCompositionBytes;
    static constexpr float kMaxHalfWidth = 320, kHalfHeight = 28, kIconWidth = 52, kCounterWidth = 64, kFontPixels = 22;
    static constexpr float kCaretBlinkSeconds = 0.53f, kSlide = 10; // kSlide: design px the bar travels while fading.
    // Storage of at least kStorageBytes that outlives the palette. Without it the palette never opens.
    void Configure(std::span<char> storage) noexcept;
    bool Available() const noexcept { return text_ != nullptr; }
    // Centred horizontally, its centre at 38 % of the height; the scale is the dock's (KineticHud::LayoutScale).
    void Layout(unsigned width, unsigned height, float dpi_scale) noexcept;
    void SetReducedMotion(bool reduced) noexcept { reduced_ = reduced; }
    bool Open(std::int64_t now) noexcept;  // Starts empty. False when unavailable.
    void Close(std::int64_t now) noexcept; // The text stays for the fade-out.
    bool IsOpen() const noexcept { return open_; }
    // Typed input. A UTF-16 code unit as WM_CHAR delivers it (surrogate halves are paired here), or a whole scalar.
    // Controls, unpaired surrogates and input beyond the capacity are dropped. True when the text changed.
    bool Utf16(std::uint16_t unit, std::int64_t now) noexcept;
    bool Scalar(std::uint32_t scalar, std::int64_t now) noexcept;
    // Committed text from an input method or the clipboard: line breaks and tabs become spaces, a NUL ends it.
    // Returns the code units taken.
    template<class Unit> std::size_t Insert(const Unit* units, std::size_t count, std::int64_t now) noexcept {
        static_assert(sizeof(Unit) == 2);
        std::size_t taken = 0;
        for (; taken < count && units[taken]; ++taken) {
            auto unit = static_cast<std::uint16_t>(units[taken]);
            if (unit == '\r' || unit == '\n' || unit == '\t') unit = ' ';
            if (!Utf16(unit, now) && Full()) break;
        }
        pending_ = 0; return taken;
    }
    // An input method's composition string (not yet committed), shown after the text; empty ends it.
    template<class Unit> void Compose(const Unit* units, std::size_t count, std::int64_t now) noexcept {
        static_assert(sizeof(Unit) == 2);
        BeginComposition(now);
        for (std::size_t i = 0; i < count && units[i]; ++i) ComposeUnit(static_cast<std::uint16_t>(units[i]));
        compose_pending_ = 0;
    }
    bool Backspace(std::int64_t now) noexcept;  // One scalar. Nothing while a composition is shown (the IME edits it).
    bool DeleteWord(std::int64_t now) noexcept; // Back to the previous space.
    std::string_view Text() const noexcept { return {text_, text_size_}; }
    std::string_view Composition() const noexcept { return {composition_, composition_size_}; }
    bool Full() const noexcept { return text_size_ + 4 > kMaxTextBytes; }
    std::uint32_t Revision() const noexcept { return revision_; } // Changes whenever the shown text does.
    // A warm session's next file (docs/session.md): revisions go on from the previous file's bar. Before any text.
    void ContinueRevision(std::uint32_t revision) noexcept { if (revision > revision_) revision_ = revision; }
    void SetHint(PaletteHint hint, unsigned matches) noexcept;
    PaletteHint Hint() const noexcept { return hint_; }
    void Rejected(std::int64_t now) noexcept; // Enter was refused: the bar stays, shakes and tints.
    void Advance(std::int64_t now) noexcept;
    const PaletteUniforms& Uniforms() const noexcept { return u_; }
    PaletteTextRect TextRect() const noexcept;
    bool Visible() const noexcept { return u_.visibility > 0.002f; }
    bool Animating() const noexcept { return open_ || Visible(); }
    bool Hit(float x, float y) const noexcept; // Inside the bar.
private:
    void BeginComposition(std::int64_t now) noexcept;
    void ComposeUnit(std::uint16_t unit) noexcept;
    PaletteUniforms u_{};
    char* text_ = nullptr;
    char* composition_ = nullptr;
    unsigned text_size_ = 0, composition_size_ = 0;
    std::uint32_t revision_ = 0;
    std::uint16_t pending_ = 0, compose_pending_ = 0; // A high surrogate waiting for its low half.
    std::int64_t last_advance_ = 0, last_input_ = 0, rejected_at_ = 0;
    float visibility_speed_ = 0;
    PaletteHint hint_ = PaletteHint::Empty;
    bool open_ = false, reduced_ = false;
};
}
