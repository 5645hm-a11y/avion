#include "media/audio/MicroVad.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::audio {
namespace {
constexpr float kFloorLimit = -80; // dBFS: below this the input is digital silence, not a noise level.
constexpr double kPi = 3.14159265358979323846;
}
bool MicroVad::Configure(core::LinearMemoryArena& arena, const VadOptions& o) noexcept {
    const auto finite = [](float v) { return std::isfinite(v); };
    if (history_ || o.sample_rate < 8000 || o.sample_rate > 192000 || o.window_ms < 10 || o.window_ms > 20 ||
        o.onset_ms < o.window_ms || o.onset_ms > 500 || o.hangover_ms < o.window_ms || o.hangover_ms > 5000 ||
        o.preroll_ms > 1000 || o.lookback_ms < o.preroll_ms + o.onset_ms || o.lookback_ms > 2000 ||
        o.max_utterance_ms < o.onset_ms + o.hangover_ms || o.max_utterance_ms > 600000 || o.warmup_ms > 5000 ||
        o.stationary_ms < 4 * o.window_ms || o.stationary_ms > 10000 ||
        !finite(o.onset_db) || !finite(o.hold_db) || o.hold_db <= 0 || o.onset_db < o.hold_db || o.onset_db > 60 ||
        !finite(o.min_level_db) || o.min_level_db > 0 || !finite(o.stationary_db) || o.stationary_db <= 0 ||
        !finite(o.floor_rise_db_s) || o.floor_rise_db_s <= 0 || !finite(o.floor_rise_speech_db_s) || o.floor_rise_speech_db_s < 0 ||
        !finite(o.zero_cross_min_hz) || !finite(o.zero_cross_max_hz) || o.zero_cross_min_hz < 0 ||
        o.zero_cross_max_hz <= o.zero_cross_min_hz) return false;
    const auto frames = [&](std::uint32_t ms) { return static_cast<std::uint64_t>(o.sample_rate) * ms / 1000; };
    const auto windows = [&](std::uint32_t ms) { return (std::max)(1u, (ms + o.window_ms - 1) / o.window_ms); };
    const auto window = static_cast<std::uint32_t>(frames(o.window_ms));
    // The history holds the lookback and the window being filled, in a power of two.
    std::uint32_t capacity = 1024;
    while (capacity < frames(o.lookback_ms) + 2ull * window) capacity *= 2;
    auto* history = static_cast<float*>(arena.Allocate(sizeof(float) * capacity));
    if (!history) return false;
    std::memset(history, 0, sizeof(float) * capacity);
    options_ = o; history_ = history; capacity_ = capacity; window_ = window;
    onset_windows_ = windows(o.onset_ms); hangover_windows_ = windows(o.hangover_ms); warmup_windows_ = o.warmup_ms / o.window_ms;
    block_windows_ = (std::max)(2u, windows(o.stationary_ms) / 2);
    lookback_ = frames(o.lookback_ms); preroll_ = frames(o.preroll_ms); longest_ = frames(o.max_utterance_ms);
    coefficient_ = static_cast<float>(1.0 - 2.0 * kPi * 80.0 / o.sample_rate);
    rise_ = o.floor_rise_db_s * static_cast<float>(o.window_ms) / 1000; rise_active_ = o.floor_rise_speech_db_s * static_cast<float>(o.window_ms) / 1000;
    Reset();
    return true;
}
void MicroVad::Reset() noexcept {
    written_ = analysed_ = 0; previous_in_ = previous_out_ = 0; floor_ = -120; warm_ = 0;
    block_fill_ = 0; have_earlier_ = false; run_ = quiet_ = 0; region_ = delivered_until_ = 0; in_region_ = open_ = false;
    current_ = {}; last_ = {}; state_ = VadState::Silence; features_ = {}; stats_ = {};
}
std::span<float> MicroVad::Acquire() noexcept {
    if (!history_) return {};
    // Up to the end of the window being filled, and of the storage: the window is analysed when it is complete,
    // before anything older than the lookback is overwritten.
    const auto position = static_cast<std::uint32_t>(written_ & (capacity_ - 1));
    const auto pending = static_cast<std::uint32_t>(written_ - analysed_);
    return {history_ + position, (std::min)(window_ - pending, capacity_ - position)};
}
VadState MicroVad::Commit(std::uint32_t frames, SpeechSink sink, void* context) noexcept {
    if (!history_ || frames > Acquire().size()) return state_;
    written_ += frames;
    if (written_ - analysed_ == window_) Analyse(sink, context);
    return state_;
}
VadState MicroVad::Process(std::span<const float> samples, SpeechSink sink, void* context) noexcept {
    while (!samples.empty()) {
        const auto room = Acquire(); if (room.empty()) break;
        const auto count = (std::min)(room.size(), samples.size());
        std::memcpy(room.data(), samples.data(), count * sizeof(float));
        (void)Commit(static_cast<std::uint32_t>(count), sink, context);
        samples = samples.subspan(count);
    }
    return state_;
}
void MicroVad::Emit(std::uint64_t begin, std::uint64_t end, VadState first, SpeechSink sink, void* context) noexcept {
    // Contiguous pieces of the history: a range that passes the end of the storage is delivered in two.
    auto state = first;
    while (begin < end) {
        const auto position = static_cast<std::uint32_t>(begin & (capacity_ - 1));
        const auto count = static_cast<std::uint32_t>((std::min)(end - begin, static_cast<std::uint64_t>(capacity_ - position)));
        if (sink) sink(context, {history_ + position, count, begin, current_.index, state});
        stats_.delivered += count; current_.delivered += count;
        begin += count; state = VadState::InUtterance;
    }
}
void MicroVad::Close(bool truncated, SpeechSink sink, void* context) noexcept {
    current_.truncated = truncated; last_ = current_;
    if (truncated) ++stats_.truncated;
    if (sink) sink(context, {nullptr, 0, analysed_, current_.index, VadState::SpeechEnd});
    open_ = in_region_ = false; run_ = quiet_ = 0; delivered_until_ = analysed_; state_ = VadState::SpeechEnd;
}
void MicroVad::Analyse(SpeechSink sink, void* context) noexcept {
    const auto begin = analysed_, end = analysed_ + window_;
    // Level and zero crossings of the window, after the DC blocker. A non-finite sample counts as zero.
    double energy = 0; std::uint32_t crossings = 0;
    float in = previous_in_, out = previous_out_;
    for (auto frame = begin; frame != end; ++frame) {
        float x = history_[frame & (capacity_ - 1)];
        if (!std::isfinite(x)) x = 0;
        const float y = x - in + coefficient_ * out;
        if ((y > 0) != (out > 0)) ++crossings;
        energy += static_cast<double>(y) * y;
        in = x; out = y;
    }
    // A denormal tail after the input stops would cost time and cross zero for ever.
    previous_in_ = in; previous_out_ = std::fabs(out) < 1e-20f ? 0.0f : out;
    analysed_ = end;
    const auto level = static_cast<float>(10.0 * std::log10(energy / window_ + 1e-12));
    const float crossing_hz = static_cast<float>(crossings) * static_cast<float>(options_.sample_rate) / (2.0f * static_cast<float>(window_));
    // Classified against the floor as it was before this window.
    const bool warming = warm_ < warmup_windows_;
    const bool active = !warming && level >= floor_ + options_.hold_db && level >= options_.min_level_db - (options_.onset_db - options_.hold_db);
    const bool voiced = active && level >= floor_ + options_.onset_db && level >= options_.min_level_db &&
                        crossing_hz >= options_.zero_cross_min_hz && crossing_hz <= options_.zero_cross_max_hz;
    // The floor: the first window sets it, warm-up follows the level both ways, then it falls at once and rises slowly.
    if (!stats_.windows) floor_ = level;
    else if (warming) floor_ += 0.25f * (level - floor_);
    else if (level < floor_) floor_ += 0.5f * (level - floor_);
    else floor_ = (std::min)(level, floor_ + (active || open_ ? rise_active_ : rise_));
    if (warming) ++warm_;
    // Stationarity: two consecutive blocks whose levels all lie within stationary_db are noise at that level.
    if (!block_fill_) block_min_ = block_max_ = level;
    else { block_min_ = (std::min)(block_min_, level); block_max_ = (std::max)(block_max_, level); }
    if (++block_fill_ == block_windows_) {
        if (have_earlier_ && (std::max)(block_max_, earlier_max_) - (std::min)(block_min_, earlier_min_) < options_.stationary_db)
            floor_ = (std::max)(floor_, (std::min)(block_min_, earlier_min_));
        earlier_min_ = block_min_; earlier_max_ = block_max_; have_earlier_ = true; block_fill_ = 0;
    }
    floor_ = (std::max)(floor_, kFloorLimit);
    features_ = {level, floor_, crossing_hz, active, voiced};
    ++stats_.windows; stats_.frames += window_; if (active) ++stats_.active_windows;
    if (!open_) {
        state_ = VadState::Silence;
        if (!active) { in_region_ = false; run_ = 0; return; }
        if (!in_region_) { in_region_ = true; region_ = begin; }
        run_ = voiced ? run_ + 1 : 0;
        if (run_ < onset_windows_) return;
        // Speech: the utterance opens before the active run, as far back as the history and the last utterance allow.
        const auto oldest = (std::max)(delivered_until_, end > lookback_ ? end - lookback_ : 0);
        const auto onset = (std::max)(region_, oldest);
        const auto first = (std::max)(onset > preroll_ ? onset - preroll_ : 0, oldest);
        current_ = {}; current_.index = ++stats_.utterances; current_.begin_frame = first; current_.onset_frame = onset;
        current_.end_frame = end; current_.active = end - onset;
        open_ = true; quiet_ = 0; state_ = VadState::SpeechStart;
        Emit(first, end, VadState::SpeechStart, sink, context);
        return;
    }
    state_ = VadState::InUtterance;
    Emit(begin, end, VadState::InUtterance, sink, context);
    if (active) { quiet_ = 0; current_.end_frame = end; current_.active += window_; }
    else if (++quiet_ >= hangover_windows_) { Close(false, sink, context); return; }
    if (end - current_.begin_frame >= longest_) Close(true, sink, context);
}
}
