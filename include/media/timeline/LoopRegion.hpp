#pragma once
#include <cstdint>
namespace media::timeline {
// A–B loop (docs/seeking.md, "A–B loop"): a stretch of the media played again and again, for close inspection, language
// practice or an instrument. Portable state only: the engine asks it what to do and seeks.
// - The L key: the first press marks A at the position, the second marks B and starts looping [A, B], the third clears.
// - Commands set either end ("set loop start", "סוף לופ") or the sentence on screen with a comfort pad on both sides.
// - B before A is taken as the other way round; a loop shorter than kMinimum is refused (a double press, not a loop).
// - While looping, Due() says when the position has reached B: the engine seeks to A. A seek outside the loop leaves it
//   set; reaching B again turns back as usual.
// Times are 100 ns of media. No allocation, no clock, one owner (the UI thread).
class LoopRegion final {
public:
    static constexpr std::int64_t kSecond = 10000000;
    static constexpr std::int64_t kCuePad = 1500000;  // 150 ms before a cue's start and after its end.
    static constexpr std::int64_t kMinimum = 2000000; // 200 ms.
    enum class State : std::uint8_t { Off, Armed, Looping }; // Armed: A is marked, B is not yet.
    enum class Step : std::uint8_t {
        MarkedStart, // A marked (Armed).
        Looping,     // [A, B] set: the engine starts it (from A when the position is not inside it).
        Cleared,     // Back to Off.
        TooShort,    // B too close to A: nothing changed.
        NoStart,     // "set loop end" with no A marked.
        Unavailable, // No duration, or a position outside the media.
    };
    Step Toggle(std::int64_t position, std::int64_t duration) noexcept;   // The L key.
    Step SetStart(std::int64_t position, std::int64_t duration) noexcept; // Marks A (a loop already set becomes Armed again).
    Step SetEnd(std::int64_t position, std::int64_t duration) noexcept;   // Marks B and loops.
    // A subtitle cue [start, end) as the loop, padded by kCuePad on both sides inside the media.
    Step SetCue(std::int64_t start, std::int64_t end, std::int64_t duration) noexcept;
    Step Clear() noexcept;
    State Current() const noexcept { return state_; }
    bool Looping() const noexcept { return state_ == State::Looping; }
    std::int64_t A() const noexcept { return a_; }
    std::int64_t B() const noexcept { return b_; }
    bool FromCue() const noexcept { return cue_; }
    // While looping: the position (media time on screen) has reached B, or the media's end inside the loop.
    bool Due(std::int64_t position) const noexcept { return state_ == State::Looping && position >= b_; }
    // While looping: the position is inside [A, B) (a seek outside the loop is followed, then turned back at B).
    bool Inside(std::int64_t position) const noexcept { return state_ == State::Looping && position >= a_ && position < b_; }
    void NoteWrap() noexcept { ++wraps_; }
    unsigned Wraps() const noexcept { return wraps_; } // Turns back to A since the loop was set.
    // The dock's bracket (HudUniforms::loop): A and B as 16-bit positions along the track, 1 (the start) to 65535 (the
    // end), in the low and high halves; 0 for an end not marked. 0: no loop.
    std::uint32_t Packed(std::int64_t duration) const noexcept;
    static std::uint16_t Fraction(std::int64_t time, std::int64_t duration) noexcept;
private:
    State state_ = State::Off;
    std::int64_t a_ = 0, b_ = 0;
    unsigned wraps_ = 0;
    bool cue_ = false;
};
}
