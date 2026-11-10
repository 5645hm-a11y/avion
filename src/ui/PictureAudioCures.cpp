#include "media/ui/PictureAudioCures.hpp"
#include <algorithm>

namespace media::ui {
namespace {
float Band(float db) { return std::clamp(db, -12.0f, 12.0f); }
}
bool PictureAudioCures::Apply(ai::IntentKind kind, Settings& now) noexcept {
    using ai::IntentKind;
    if (kind != IntentKind::CureDark && kind != IntentKind::CureNoise && kind != IntentKind::CureMuffled) return false;
    before_ = now; saved_ = true; last_ = kind;
    auto& p = now.picture; auto& a = now.audio;
    switch (kind) {
    case IntentKind::CureDark:
        p.shadow_lift = (std::max)(p.shadow_lift, 0.6f);
        p.gamma = (std::max)(p.gamma, 1.15f);
        p.brightness = (std::min)(p.brightness + 0.03f, 0.1f);
        break;
    case IntentKind::CureNoise:
        a.notch_killer = 1;
        a.eq_db[0] = (std::min)(a.eq_db[0], -6.0f);
        break;
    default: // CureMuffled.
        a.eq_db[6] = Band(a.eq_db[6] + 4.0f); a.eq_db[7] = Band(a.eq_db[7] + 3.0f); a.eq_db[5] = Band(a.eq_db[5] + 2.0f);
        a.eq_db[3] = Band(a.eq_db[3] - 2.0f);
        a.clarity = (std::max)(a.clarity, 2u);
        a.speech_ducking = 1;
        break;
    }
    return true;
}
bool PictureAudioCures::Undo(Settings& now) noexcept {
    if (!saved_) return false;
    now = before_; saved_ = false;
    return true;
}
const char* PictureAudioCures::Describe(ai::IntentKind kind) noexcept {
    switch (kind) {
    case ai::IntentKind::CureDark: return "Shadows lifted \xc2\xb7 highlights kept \xc2\xb7 say \xe2\x80\x9cundo\xe2\x80\x9d to put it back";
    case ai::IntentKind::CureNoise: return "Hum and drones notched out \xc2\xb7 rumble cut \xc2\xb7 say \xe2\x80\x9cundo\xe2\x80\x9d to put it back";
    case ai::IntentKind::CureMuffled: return "Dialogue brought forward \xc2\xb7 presence +4 dB \xc2\xb7 say \xe2\x80\x9cundo\xe2\x80\x9d to put it back";
    default: return "";
    }
}
}
