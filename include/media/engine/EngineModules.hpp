#pragma once
#include <cstdint>

namespace media::engine {
// Engine modules (docs/sdk.md, "Modules"): what a host can leave out at initialisation. A module left out takes no arena
// memory and starts no thread. Each enabled module takes one fixed region of the file arena, made in Preparing before
// anything else (a nested core::LinearMemoryArena view): every allocation of the module comes from its region, and a module
// that needs more than it holds is disabled for that file (each one is optional: playback goes on without it). So the
// arena's capacity is exactly kCoreArenaBytes plus the enabled regions (ArenaBudget), and leaving a module out removes
// exactly its region from both the capacity and the bytes in use.
// The same values are the C SDK's MEDIA_MODULE_* (include/media/c_api.h).
enum EngineModuleFlags : std::uint32_t {
    kModuleVoice = 1u << 0,          // Capture, MicroVad, the recogniser and the wake gate: VoiceTexts ring, VAD history,
                                     // recogniser tables and scratch, the capture's buffers; the voice thread.
    kModuleHud = 1u << 1,            // The dock, the command bar (CommandPalette text, its text thread) and status cues (their
                                     // text thread).
    kModuleSubtitles = 1u << 2,      // Sidecar and embedded timed text, SubtitleTrack, quote and concept search, the SDF glyph
                                     // pipeline (its shaping thread).
    kModuleNeuralUpscale = 1u << 3,  // Neural 2x luma: GPU resources only (no arena region, no thread).
    kModuleChapters = 1u << 4,       // Chapters, structural scenes, the keyframe timeline.
    kModuleTelemetry = 1u << 5,      // Frame signatures (their ring) and the DSP's spectral analysis that feeds them; perception
                                     // on the tracker's grids (the objects "next target" moves between, 2026-10-31).
    kAllModules = (1u << 6) - 1u,
};
inline constexpr unsigned kModuleCount = 6;
struct ModuleBudget { std::uint64_t arena_bytes = 0; std::uint32_t threads = 0; };
// The core: 16 file blocks of 8 MiB (or the network's chunks), the demux index pool (ParserLimits::index_bytes, 48 MiB),
// codec tables, queues, the audio path's buffers. Measured 2026-10-21: 177.0 MiB on every fixture (4K HEVC, 7.1, the 3.5-hour
// file, the A-B loop's pinned PCM), so 192 MiB leaves 15 MiB.
inline constexpr std::uint64_t kCoreArenaBytes = 192ull << 20;
// Each module's region (multiples of 64 KiB, so that leaving one out moves later allocations by whole pages and changes no
// alignment padding) and the threads it may start while open. Measured use (docs/sdk.md, "Modules"):
inline constexpr ModuleBudget kModuleBudgets[kModuleCount]{
    {12ull << 20, 1},  // Voice: 10.26 MiB (VAD, recogniser tables and scratch) + the capture ring (256 KiB).
    {3ull << 19, 2},   // HUD: 1.25 MiB (command bar text, both text pipelines' staging); a warm session's pipelines use its own.
    {56ull << 20, 1},  // Subtitles: 9.6 MiB of glyph pipeline, then the text (sidecars up to about 6 MiB; more is refused); API 17: 24 MiB
                       // more for subtitles after open (a 4 MiB read buffer and two 10 MiB states, double-buffered).
    {0, 0},            // Neural upscale: GPU resources only.
    {3ull << 19, 0},   // Chapters: at most 1.25 MiB (65,536 keyframes, 16,384 dialogue spans).
    {1408ull << 10, 1}, // Telemetry: the 64-signature ring (4.2 KiB) and perception on the tracker's grids (at most 480 cells
                        // wide: 324.5 KiB, video::Perception::ArenaBytes; 2026-10-31), the grid handed to its thread (at most 127.5 KiB)
                        // and that thread (2026-11-01); the event camera on the grids (at most 766 KiB: two planes of 255 KiB and its
                        // 32,768 events in 256 KiB) and their virtual IMU (at most 245 KiB, 159 KiB at 480 x 272) (2026-11-03; 3,392 KiB
                        // until 2026-11-04: docs/picture.md, "The telemetry module's memory").
};
[[nodiscard]] constexpr int ModuleIndex(std::uint32_t module) noexcept {
    for (unsigned i = 0; i < kModuleCount; ++i) if (module == (1u << i)) return static_cast<int>(i);
    return -1;
}
[[nodiscard]] constexpr std::uint64_t ArenaBudget(std::uint32_t modules) noexcept {
    std::uint64_t bytes = kCoreArenaBytes;
    for (unsigned i = 0; i < kModuleCount; ++i) if (modules & (1u << i)) bytes += kModuleBudgets[i].arena_bytes;
    return bytes;
}
static_assert(ArenaBudget(0) == kCoreArenaBytes && ArenaBudget(kAllModules) > ArenaBudget(kAllModules & ~kModuleVoice));
}
