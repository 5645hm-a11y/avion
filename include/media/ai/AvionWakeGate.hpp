#pragma once
#include "media/audio/MicroVad.hpp"
#include <atomic>
#include <cstdint>
#include <string_view>

namespace media::ai {
// What the wake gate made of one recognised utterance.
enum class WakeDecision : std::uint8_t {
    Command,        // "Avion, pause the movie" in one breath: the command after the name is handed on.
    Listening,      // "Avion" alone: the listening window opened (or opened again).
    WindowCommand,  // A command without the name, inside an open window: handed on, and the window closes.
    Ignored,        // No name and no open window: room talk. Nothing is done.
};
const char* Name(WakeDecision) noexcept;
struct WakeOptions {
    std::uint32_t sample_rate = 16000; // Of the utterance positions (the capture stream).
    std::uint32_t window_ms = 4000;    // After "avion" alone: how long a command without the name is still taken.
    bool required = true;              // False: every utterance is a command (the name is only stripped).
    // With `required` false: an utterance is a command only while the host's talk button is held (SetTalk); otherwise ignored.
    bool push_to_talk = false;
};
struct WakeStatistics {
    std::uint32_t utterances = 0, commands = 0, wakes = 0, window_commands = 0, ignored = 0;
    std::uint32_t expired = 0; // Utterances that came after an opened window had closed by time.
};
// Called for every decision but Ignored: the command's text (empty for Listening), valid during the call.
using WakeHandler = void (*)(void* context, WakeDecision, std::string_view command, const audio::Utterance&) noexcept;

// The wake word (docs/voice.md, "Wake word"): the player acts on speech only when it is addressed by its name, "Avion".
// People talk to each other in a living room; a command word in their talk ("then she said pause") must not stop
// the film. It sits between VoiceGate (the recognised text of each utterance) and LocalIntentDispatcher:
//
//   VoiceGate -> AcousticIntentRecognizer (writes "avion" first when it heard the name) -> AvionWakeGate -> Dispatch
//
// - "Avion <command>" (one breath, or typed: "Avion, אשמח שתמשיך 10 שניות קדימה"): the command runs.
// - "Avion" alone: a window of window_ms opens from the end of that utterance; the next command said without the name
//   and starting inside it runs, and closes the window. The player shows a silent "Avion · Listening…" cue meanwhile
//   (no spoken reply). Saying the name again opens a new window.
// - Anything else is ignored.
// It reads text, not sound, so it works with any recogniser that writes the name, and with typed text. Time is the
// capture stream's (the detector's utterance positions), not a clock: decisions depend on the samples alone.
// Fixed state, no allocation, one thread (VoiceGate's).
class AvionWakeGate final {
public:
    void Configure(const WakeOptions& options, WakeHandler handler, void* context) noexcept;
    // VoiceGate's UtteranceHandler (`context`: the gate).
    static void Handle(void* context, std::string_view text, const audio::Utterance& utterance) noexcept;
    // The same by hand: the decision, and the command to run (empty for Listening and Ignored).
    WakeDecision Decide(std::string_view text, const audio::Utterance& utterance, std::string_view& command) noexcept;
    // The name at the start of `text` ("avion", any case, or the Hebrew spelling אביון), then the end, a space or
    // punctuation; `rest` is what follows, separators skipped.
    [[nodiscard]] static bool StripWake(std::string_view text, std::string_view& rest) noexcept;
    // A custom name instead of "Avion" (docs/voice.md, "Wake word"; the SDK's MEDIA_WAKE_CUSTOM): UTF-8, at most kMaxWord
    // bytes, matched like the default (a whole word or words at the start, ASCII letters in any case). Empty: "Avion" again.
    // False (and the name unchanged) for a name that is too long or blank. The recogniser writes only words of its vocabulary:
    // a name outside it is still matched in text given to Decide (a host's own recognition, typed commands).
    static constexpr std::size_t kMaxWord = 31;
    bool SetWord(std::string_view word) noexcept;
    [[nodiscard]] std::string_view Word() const noexcept { return {word_, word_bytes_}; } // Empty: "Avion".
    [[nodiscard]] bool StripName(std::string_view text, std::string_view& rest) const noexcept; // The configured name.
    // The policy without reconfiguring (the window closes; statistics kept). The gate's thread only.
    void SetPolicy(bool required, bool push_to_talk) noexcept { options_.required = required; options_.push_to_talk = push_to_talk; open_ = false; }
    // Push-to-talk (WakeOptions::push_to_talk): whether the host's talk button is held now. Any thread.
    void SetTalk(bool held) noexcept { talk_.store(held, std::memory_order_relaxed); }
    [[nodiscard]] bool Talking() const noexcept { return talk_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool Listening() const noexcept { return open_; }
    [[nodiscard]] std::uint64_t WindowEnd() const noexcept { return window_end_; } // Capture frame the window closes at.
    // Whether the window is still open at capture frame `frame` (an utterance starting there would be taken).
    [[nodiscard]] bool OpenAt(std::uint64_t frame) const noexcept { return open_ && frame <= window_end_; }
    void Reset() noexcept { open_ = false; window_end_ = 0; }
    [[nodiscard]] const WakeStatistics& Stats() const noexcept { return stats_; }
    [[nodiscard]] const WakeOptions& Options() const noexcept { return options_; }
private:
    WakeOptions options_{};
    WakeHandler handler_ = nullptr; void* context_ = nullptr;
    std::uint64_t window_frames_ = 64000, window_end_ = 0;
    bool open_ = false;
    std::atomic<bool> talk_{false}; // Written by the host's thread, read by the gate's (relaxed: a flag, nothing to order).
    char word_[kMaxWord + 1]{};
    std::size_t word_bytes_ = 0;
    WakeStatistics stats_{};
};
}
