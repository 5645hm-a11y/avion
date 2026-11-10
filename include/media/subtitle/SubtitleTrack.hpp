#pragma once
#include "media/core/linear_memory_arena.hpp"
#include "media/text/Utf8.hpp"
#include <atomic>
#include <span>
#include <string_view>

namespace media::subtitle {
struct Cue {
    std::int64_t start = 0, end = 0, prefix_end = 0; // Half-open interval, 100 ns.
    std::string_view text; // Immutable UTF-8 view; no copies while parsing/selecting.
    std::uint8_t channel = 0; // Which source (subtitle track) the cue came from.
};
enum class ParseStatus { CueReady, End, Invalid };
// Incremental cursor over an immutable buffer (not a network decoder). Next allocates nothing.
class CueReader final {
public:
    explicit CueReader(std::string_view source) noexcept;
    ParseStatus Next(Cue&) noexcept;
    // After Invalid: skip the rest of the broken cue block (up to its blank line) so Next can continue.
    void Resync() noexcept;
    unsigned Line() const noexcept { return line_; }
private:
    std::string_view ReadLine() noexcept;
    std::string_view source_;
    std::size_t position_ = 0;
    unsigned line_ = 0;
    bool vtt_ = false, valid_ = true, blank_ = true; // blank_: the last line read was empty.
};
using text::Utf8; // Strict UTF-8 decoding of one scalar.
// Tolerant track: malformed cue blocks are skipped (counted), cues beyond kMaxCues are dropped, and more than
// kMaxActive overlapping cues are accepted; Select then returns the kMaxActive latest-starting ones.
// Prepare fails only when no valid cue remains, the source is empty/over 8 MiB, or the arena is exhausted.
// Several subtitle tracks (languages) share one track as channels: every cue keeps its channel, and only the cues of
// the channels shown are selected, drawn and searched. Switching is one atomic store from any thread; nothing is
// shaped, allocated or reordered for it.
class SubtitleTrack final {
public:
    static constexpr unsigned kMaxCues = 16384, kMaxActive = 4, kMaxTextBytes = 4096, kMaxChannels = 16;
    struct Statistics { unsigned skipped = 0, dropped = 0, max_active = 0; };
    SubtitleTrack() noexcept = default;
    SubtitleTrack(const SubtitleTrack&) = delete;
    SubtitleTrack& operator=(const SubtitleTrack&) = delete;
    // Preparing only: copy the source(s) and allocate the exact cue table in the caller's arena. Source i is channel i;
    // a source with no valid cue is an empty channel. Fails when no source has a cue.
    bool Prepare(core::LinearMemoryArena&, std::string_view source) noexcept;
    bool Prepare(core::LinearMemoryArena&, std::span<const std::string_view> sources) noexcept;
    unsigned Select(std::int64_t time, std::span<unsigned> indices) const noexcept;
    std::span<const Cue> Cues() const noexcept { return {cues_, count_}; }
    unsigned ErrorLine() const noexcept { return error_line_; } // First malformed line (1-based), 0 if none.
    const Statistics& Stats() const noexcept { return stats_; }
    unsigned Channels() const noexcept { return channels_; }
    unsigned ChannelCues(unsigned channel) const noexcept { return channel < kMaxChannels ? per_channel_[channel] : 0; }
    // The channels shown, one bit each (all of them after Prepare; 0: none).
    void Show(std::uint32_t mask) noexcept { shown_.store(mask, std::memory_order_relaxed); }
    std::uint32_t Shown() const noexcept { return shown_.load(std::memory_order_relaxed); }
    bool Shown(unsigned cue) const noexcept { return cue < count_ && ((Shown() >> cues_[cue].channel) & 1u) != 0; }
private:
    Cue* cues_ = nullptr;
    unsigned count_ = 0, error_line_ = 0, channels_ = 0, per_channel_[kMaxChannels]{};
    std::atomic<std::uint32_t> shown_{~0u};
    Statistics stats_{};
};
}
