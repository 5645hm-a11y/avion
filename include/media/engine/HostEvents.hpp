#pragma once
// Embedding (API 12; docs/sdk.md, "Embedding: API 12"): a command posted by the host's thread and what the engine reports back,
// through single-producer single-consumer rings (core::SpscRingBuffer), and the state a host may read at any time. Portable:
// the windowed engine's PlaybackCoordinator and the C SDK's host mode use the same types.
#include <cstdint>

namespace media::engine {
struct HostCommand {              // 256 bytes.
    char text[232];               // UTF-8, NUL-terminated (op 0).
    std::uint32_t id;             // The host's, returned in its kCommand event.
    std::uint32_t length;
    std::uint32_t op;             // API 15: HostControl (HostControl.hpp); 0: the text is a command of the grammar.
    std::uint32_t reserved;
    double value;                 // The control's value (seconds, 0..1, a speed, +-1).
};
static_assert(sizeof(HostCommand) == 256, "a ring slot of 256 bytes");
struct HostEvent {                // Trivial (a ring's slot): made with HostEvent{}.
    enum Kind : std::uint32_t { kOpened = 1, kState = 2, kPosition = 3, kSeekLanded = 4, kCommand = 5, kFullscreen = 6, kEnded = 7 };
    enum Reason : std::uint32_t { kMedia = 0, kStopped = 1, kClosed = 2, kError = 3, kLimit = 4 };
    std::uint32_t kind, reason;
    std::int64_t time, position, duration, latency;
    std::uint32_t id;
    std::int32_t status;      // kCommand: 0 Ok, else refused; kEnded: 0 ok, else the playback failed.
    std::int32_t paused, muted;
    float volume, speed;
    std::uint32_t width, height;
    std::uint32_t lost;       // Events dropped before this one (the ring was full).
};
// What the host may read at any time: the engine's thread stores it at every pass.
struct EmbedState {
    std::int64_t position = 0, duration = 0;
    bool paused = false, muted = false;
    float volume = 0, speed = 0;
    std::uint32_t presented = 0, dropped = 0;
};
}
