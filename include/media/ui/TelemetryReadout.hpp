#pragma once
#include "media/core/flight_recorder.hpp"
#include "media/render/QualityGovernor.hpp"
#include <cstdint>
namespace media::ui {
// What the engine measures now (docs/session.md, "Telemetry readout"): every field is read from the engine's own state
// (the quality governor, the renderer's counters, the audio routing, the flight recorders), never estimated. Filled by
// the engine on the UI thread; the readout only formats it.
struct TelemetrySnapshot {
    std::int64_t now = 0; // The engine's clock (100 ns), the same one the events and the governor's changes are stamped with.
    // Picture quality (render::QualityGovernor).
    render::QualityMode mode = render::QualityMode::Auto;
    render::QualityTier tier = render::QualityTier::EdgeDirected;
    bool timing = false;                  // GPU timestamps measure the conversions.
    bool neural_available = false;        // The Neural tier can run on this picture now.
    std::int64_t p95 = 0, budget = 0;     // The current tier's p95 GPU time and the frame interval (100 ns; 0: none yet).
    unsigned samples = 0;                 // Pictures measured at the current tier (its p95's window; 0: none yet).
    unsigned changes = 0;                 // Tier changes so far.
    render::QualityGovernor::Change last_change{};
    float step_down = 0.85f, step_up = 0.55f; // The governor's thresholds (GovernorOptions).
    // Picture.
    bool picture = false;                 // False: music mode (no picture track).
    const char* codec = "";               // "H.264", "HEVC".
    unsigned bits = 8;
    const char* transfer = "";            // "PQ", "HLG", or "" for SDR.
    unsigned source_width = 0, source_height = 0, shown_width = 0, shown_height = 0;
    float zoom = 1;
    // Sound.
    bool audio = false, audio_unsupported = false;
    unsigned sample_rate = 0, source_channels = 0, output_channels = 0;
    bool fold_down = false, binaural = false;
    float volume = 1, speed = 1;
    bool muted = false;
    // Loudness (BS.1770, audio::NightMode's meters): of the programme, and night mode's gain.
    bool loudness = false, night = false;
    float lufs_short = -144, true_peak = -144, night_gain = 0;
    // Presentation.
    std::uint64_t presented = 0, dropped = 0;
    std::uint8_t drop_waited = 0;         // What the last dropped picture waited for (D3D12Renderer::Wait mask).
    std::uint64_t tiles_reused = 0, tiles_evaluated = 0; // Neural temporal tile reuse.
    // A–B loop.
    bool loop = false;
    std::int64_t loop_a = 0, loop_b = 0;
    unsigned loop_wraps = 0;
    // The newest flight recorder events of both threads, oldest first.
    const core::FlightRecord* events = nullptr;
    unsigned event_count = 0;
};
// The readout's lines are separated by '\n'; at most kMaxLines of them. Nothing is cut inside a character: a part that
// does not fit is left out whole. Both return the bytes written (NUL-terminated within `capacity`).
inline constexpr unsigned kReadoutLines = 8;
// The telemetry panel (I, "show stats", "הצג נתונים"): quality and why, picture, sound, frames, loop, the last event.
unsigned ComposeStats(const TelemetrySnapshot&, char* out, unsigned capacity) noexcept;
// "Why did quality change?", "למה האיכות ירדה?": the tier on screen, since when, and the measurement that decided it.
unsigned ExplainQuality(const TelemetrySnapshot&, char* out, unsigned capacity) noexcept;
// One event as a phrase ("Seek landed at 0:12.4 in 38 ms"), for the readout's last line and for logs.
unsigned DescribeEvent(const core::FlightRecord&, std::int64_t now, char* out, unsigned capacity) noexcept;
}
