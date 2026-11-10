#pragma once
#include "media/core/linear_memory_arena.hpp"
#include <cstdint>
#include <span>

namespace media::audio {
// What one analysis window did to the utterance state machine:
//   Silence -> SpeechStart -> InUtterance ... -> SpeechEnd -> Silence
enum class VadState : std::uint8_t {
    Silence,     // No utterance: the window was discarded.
    SpeechStart, // Speech was confirmed in this window: the utterance opens, with the audio leading up to it.
    InUtterance, // The utterance continues (speech, or a pause shorter than the hangover).
    SpeechEnd,   // The hangover ran out (or the utterance reached its longest length) in this window.
};
struct VadOptions {
    std::uint32_t sample_rate = 16000;     // 8000 to 192000 (the capture falls back to the endpoint's rate).
    std::uint32_t window_ms = 10;          // 10 to 20: the analysis window.
    std::uint32_t onset_ms = 40;           // Speech is confirmed by this much of it in a row (shorter sounds: clicks).
    std::uint32_t hangover_ms = 250;       // A pause this long ends the utterance (shorter pauses stay inside it).
    std::uint32_t preroll_ms = 100;        // Audio kept from before the first active window (weak consonants).
    std::uint32_t lookback_ms = 400;       // The most audio an utterance can open with (pre-roll, onset, a weak lead-in).
    std::uint32_t max_utterance_ms = 12000; // A longer utterance is ended (`truncated`) and a new one may begin.
    std::uint32_t warmup_ms = 300;         // After Configure/Reset: the noise floor settles, nothing is detected.
    std::uint32_t stationary_ms = 600;     // A level that stays within `stationary_db` this long is noise, not speech.
    float onset_db = 12;                   // Above the noise floor to confirm speech (with the zero-crossing band).
    float hold_db = 6;                     // Above the noise floor to stay in (or lead into) an utterance: hysteresis.
    float min_level_db = -55;              // dBFS: nothing quieter confirms speech, whatever the floor.
    float stationary_db = 5;
    float floor_rise_db_s = 10;            // How fast the floor follows louder noise while silent,
    float floor_rise_speech_db_s = 2;      // and while something is active (it falls at once, in either case).
    float zero_cross_min_hz = 70;          // Voiced speech crosses zero at the rate of its dominant component, as a
    float zero_cross_max_hz = 2400;        // frequency between these: the pitch itself (80 Hz up) in a nasal or a closed
                                           // vowel, the first formant in an open one. Below: mains hum, thumps. Above:
                                           // hiss and broadband noise.
};
// A run of speech samples, contiguous in memory and valid only during the call that delivers it. An utterance is
// its SpeechStart slice (which carries the pre-roll), any number of InUtterance slices and one empty SpeechEnd slice.
struct SpeechSlice {
    const float* samples = nullptr;
    std::uint32_t frames = 0;
    std::uint64_t first_frame = 0;  // Position in the stream given to the detector since Configure/Reset.
    std::uint32_t utterance = 0;    // 1, 2, ...
    VadState state = VadState::Silence;
};
struct Utterance {
    std::uint32_t index = 0;
    std::uint64_t begin_frame = 0;  // First frame delivered (pre-roll included).
    std::uint64_t onset_frame = 0;  // Start of the first active window.
    std::uint64_t end_frame = 0;    // End of the last active window.
    std::uint64_t delivered = 0;    // Frames delivered: from begin_frame through the hangover.
    std::uint64_t active = 0;       // Frames in active windows (a consumer may ignore a very short utterance).
    bool truncated = false;         // Ended by max_utterance_ms, not by a pause.
};
struct VadFeatures { float level_db = -120, floor_db = -120, zero_cross_hz = 0; bool active = false, voiced = false; };
struct VadStatistics {
    std::uint64_t frames = 0, delivered = 0; // Frames analysed; frames handed on as speech. The rest was discarded.
    std::uint32_t windows = 0, active_windows = 0, utterances = 0, truncated = 0;
};
using SpeechSink = void (*)(void* context, const SpeechSlice&) noexcept;

// Voice activity detection small enough to run on every capture window (docs/voice.md): short-time energy against
// an adaptive noise floor, and the zero-crossing rate, on 10 to 20 ms windows of mono float PCM. It decides only
// where an utterance starts and ends, so that silence and steady noise never reach a recogniser; it does not tell
// speech from other modulated sound in the voiced band (music, a second talker, a television).
// - Energy is measured after a first-order DC blocker (80 Hz), in dB.
// - The floor falls to a quieter window at once and rises slowly; a level that stays within a few dB for
//   `stationary_ms` becomes the floor whatever it is, since speech never holds still that long. A window is *active*
//   above floor + hold_db, and *voiced* above floor + onset_db and min_level_db with zero crossings in the voiced
//   band.
// - `onset_ms` of voiced windows in a row open an utterance, which starts `preroll_ms` before the first active
//   window of that run (bounded by `lookback_ms` and by the previous utterance). Active windows keep it open;
//   `hangover_ms` without one closes it.
// Deterministic: the result depends on the samples only, not on how they are split between calls. One owner thread
// (the capture ring's reader). After Configure: fixed storage from the arena, no allocation, no locks, no clock.
class MicroVad final {
public:
    [[nodiscard]] bool Configure(core::LinearMemoryArena&, const VadOptions& = {}) noexcept;
    // Feeds samples. `sink` is called, inside this call, with each slice of each utterance; it may be null
    // (detection only). Returns the state after the last whole window.
    VadState Process(std::span<const float> samples, SpeechSink sink, void* context) noexcept;
    // The same without a copy: fill Acquire() (never empty after Configure: at most the rest of the current window),
    // then Commit the count written.
    [[nodiscard]] std::span<float> Acquire() noexcept;
    VadState Commit(std::uint32_t frames, SpeechSink sink, void* context) noexcept;
    // Everything a source holds now, read straight into the detector: anything with
    // `std::uint32_t Read(std::span<float>)`, such as Win32WasapiCapture. Returns the frames taken.
    template <class Source> std::uint64_t Drain(Source& source, SpeechSink sink, void* context) noexcept {
        std::uint64_t total = 0;
        for (;;) {
            const auto room = Acquire(); if (room.empty()) break;
            const std::uint32_t read = source.Read(room); if (!read) break;
            (void)Commit(read, sink, context); total += read;
        }
        return total;
    }
    // A gap in the stream (a new capture): forgets the floor and an open utterance, whose SpeechEnd is not sent.
    void Reset() noexcept;
    [[nodiscard]] VadState State() const noexcept { return state_; }
    [[nodiscard]] bool InUtterance() const noexcept { return open_; }
    [[nodiscard]] const Utterance& Last() const noexcept { return last_; }        // The last utterance closed.
    [[nodiscard]] const VadFeatures& Features() const noexcept { return features_; } // Of the last window.
    [[nodiscard]] const VadStatistics& Stats() const noexcept { return stats_; }
    [[nodiscard]] const VadOptions& Options() const noexcept { return options_; }
    [[nodiscard]] std::uint32_t WindowFrames() const noexcept { return window_; }
private:
    void Analyse(SpeechSink sink, void* context) noexcept;
    void Emit(std::uint64_t begin, std::uint64_t end, VadState first, SpeechSink sink, void* context) noexcept;
    void Close(bool truncated, SpeechSink sink, void* context) noexcept;
    VadOptions options_{};
    float* history_ = nullptr;      // The newest capacity_ samples, by stream position.
    std::uint32_t capacity_ = 0, window_ = 0;
    std::uint32_t onset_windows_ = 0, hangover_windows_ = 0, warmup_windows_ = 0, block_windows_ = 0;
    std::uint64_t lookback_ = 0, preroll_ = 0, longest_ = 0;
    float coefficient_ = 0, rise_ = 0, rise_active_ = 0;
    std::uint64_t written_ = 0, analysed_ = 0;
    float previous_in_ = 0, previous_out_ = 0; // DC blocker; the sign of previous_out_ carries across windows.
    float floor_ = -120;
    std::uint32_t warm_ = 0;
    // Stationarity: level extremes of the block being filled and of the one before it.
    float block_min_ = 0, block_max_ = 0, earlier_min_ = 0, earlier_max_ = 0;
    std::uint32_t block_fill_ = 0; bool have_earlier_ = false;
    // Onset: voiced windows in a row, and where the run of active windows containing them began.
    std::uint32_t run_ = 0, quiet_ = 0;
    std::uint64_t region_ = 0, delivered_until_ = 0;
    bool in_region_ = false, open_ = false;
    Utterance current_{}, last_{};
    VadState state_ = VadState::Silence;
    VadFeatures features_{};
    VadStatistics stats_{};
};
}
