#pragma once
// Structured controls (API 15; docs/sdk-reference.md, "Controls"): media_engine_post_control's operations as the intents the
// grammar's commands become, so that a host's slider and a spoken "volume 40" run the same code. Portable; both engines use it.
#include "media/ai/LocalIntentDispatcher.hpp"
#include <cmath>
#include <cstdint>

namespace media::engine {
enum class HostControl : std::uint32_t { kText = 0, kSeek = 1, kPause = 2, kVolume = 3, kMute = 4, kSpeed = 5, kStepFrame = 6, kSeekRelative = 7,
                                         kAudioTrack = 8, kSubtitleTrack = 9 }; // API 16: a track by its id (subtitles: -1 none, 0 the sidecar).
constexpr std::uint32_t kLastControl = 9;

// The value's valid range for a control (checked when it is posted: a bad value is refused at once, not later in an event).
inline bool ControlValueValid(HostControl op, double value) noexcept {
    if (!std::isfinite(value)) return false;
    switch (op) {
    case HostControl::kSeek: return value >= 0 && value <= 86400.0 * 366;
    case HostControl::kSeekRelative: return std::fabs(value) <= 86400.0 * 366;
    case HostControl::kPause: case HostControl::kMute: return value == 0 || value == 1;
    case HostControl::kVolume: return value >= 0 && value <= 2; // API 16: above 1, a boost to x2 through a soft limiter.
    case HostControl::kSpeed: return value >= 0.25 && value <= 4.0; // API 16: to 4x.
    case HostControl::kStepFrame: return value == 1 || value == -1;
    case HostControl::kAudioTrack: return value >= 1 && value <= 4294967295.0 && value == std::floor(value);
    case HostControl::kSubtitleTrack: return value >= -1 && value <= 4294967295.0 && value == std::floor(value);
    case HostControl::kText: break;
    }
    return false;
}
// The intent for a control, given the position and duration now (100 ns; duration 0: unknown). A seek past either end lands on
// it (a slider's ends are the media's). False for kStepFrame (not an intent: the engine steps itself) and kText.
inline bool ControlIntent(HostControl op, double value, std::int64_t position, std::int64_t duration, ai::Intent& intent) noexcept {
    intent = {};
    const auto clamp = [&](double t) {
        if (t < 0) t = 0;
        if (duration > 0 && t > static_cast<double>(duration - 1)) t = static_cast<double>(duration - 1);
        return static_cast<std::int64_t>(t);
    };
    switch (op) {
    case HostControl::kSeek: intent.kind = ai::IntentKind::SeekAbsolute; intent.target_100ns = clamp(value * 1e7); return true;
    case HostControl::kSeekRelative:
        intent.kind = ai::IntentKind::SeekRelative; intent.offset_100ns = static_cast<std::int64_t>(value * 1e7);
        intent.target_100ns = clamp(static_cast<double>(position) + value * 1e7); return true;
    case HostControl::kPause: intent.kind = value != 0 ? ai::IntentKind::Pause : ai::IntentKind::Play; return true;
    case HostControl::kVolume: intent.kind = ai::IntentKind::VolumeSet; intent.volume = static_cast<float>(value); return true;
    case HostControl::kMute: intent.kind = value != 0 ? ai::IntentKind::Mute : ai::IntentKind::Unmute; return true;
    case HostControl::kSpeed: intent.kind = ai::IntentKind::SpeedSet; intent.speed = static_cast<float>(value); return true;
    case HostControl::kStepFrame: case HostControl::kAudioTrack: case HostControl::kSubtitleTrack: case HostControl::kText: break;
    }
    return false;
}
}
