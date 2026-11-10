#pragma once
#include "media/ai/LocalIntentDispatcher.hpp"
#include <cstdint>

namespace media::ui {
// Symptom to cure (docs/intents.md, "Cures"): a viewer names what is wrong ("too dark", "חשוך", "noisy", "יש זמזום",
// "muffled", "הדיבור נבלע") and the engine changes the settings that remedy it; "undo" / "בטל" puts back exactly what was there
// before the last cure (one step). The settings are plain data the engine reads from its renderer and DSP controls and writes
// back; this class decides the remedy and keeps the one snapshot. Portable, fixed storage, one thread (the UI's).
// - CureDark: shadow lift to at least 0.6, gamma to at least 1.15, brightness +0.03 (at most 0.1): the shadows come up,
//   highlights stay where they are (docs/picture.md, "Picture controls").
// - CureNoise: the notch killer on (stationary drones and hum notched out) and the 31.5 Hz band at -6 dB at most (rumble).
// - CureMuffled: 2 kHz +4 dB, 4 kHz +3 dB, 1 kHz +2 dB, 250 Hz -2 dB (mud), dialogue clarity at least 2, speech-protected ducking
//   on (docs/audio-dsp.md, "Mastering EQ and audio zoom"). Every band stays within +-12 dB.
// Every field is four bytes: the settings compare bit for bit (no padding).
class PictureAudioCures final {
public:
    struct Picture {
        float brightness = 0, contrast = 1, saturation = 1, gamma = 1, shadow_lift = 0, micro_motion = 0;
    };
    struct Audio {
        float eq_db[10]{};
        std::uint32_t clarity = 0, notch_killer = 0, speech_ducking = 0, iso_loudness = 0, audio_zoom = 0;
    };
    struct Settings { Picture picture; Audio audio; };
    static_assert(sizeof(Picture) == 24 && sizeof(Audio) == 60 && sizeof(Settings) == 84, "no padding");
    // Applies the cure for `kind` to `now` and keeps what was there before (replacing an earlier snapshot). False: `kind` is no cure.
    bool Apply(ai::IntentKind kind, Settings& now) noexcept;
    // Puts back the settings from before the last cure. False: nothing to undo (none applied, or already undone).
    bool Undo(Settings& now) noexcept;
    [[nodiscard]] bool CanUndo() const noexcept { return saved_; }
    [[nodiscard]] ai::IntentKind Last() const noexcept { return last_; }
    void Reset() noexcept { saved_ = false; last_ = ai::IntentKind::None; } // New media.
    // The status cue that says what a cure did ("Shadows lifted · say “undo” to put it back").
    static const char* Describe(ai::IntentKind kind) noexcept;
private:
    Settings before_{};
    bool saved_ = false;
    ai::IntentKind last_ = ai::IntentKind::None;
};
}
