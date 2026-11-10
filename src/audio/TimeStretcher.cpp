#include "media/audio/TimeStretcher.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::audio {
namespace {
constexpr double kPi = 3.14159265358979323846;
// The oldest input frame still needed: the next to copy, or the next frame's template and earliest candidate start.
long long Oldest(long long read, bool passing, long long previous, double analysis, double hop_in, unsigned hop, unsigned search) noexcept {
    if (passing) return read;
    const auto next = static_cast<long long>(std::floor(analysis + hop_in)) - static_cast<long long>(search) - 1;
    return (std::min)(previous + static_cast<long long>(hop), next);
}
// The last frame start Best may choose for `target`: within the search, never before the stream's first frame (a stretch
// that begins within a hop of the start has a target below zero).
long long LastCandidate(long long target, unsigned search) noexcept { return (std::max)(target + static_cast<long long>(search), 0LL); }
// Whole hops the input up to `end` allows from an analysis position (one at most when ending: then it passes on).
unsigned Hops(double analysis, double hop_in, long long end, unsigned search, unsigned window, bool ending) noexcept {
    unsigned hops = 0;
    while (hops < 64) {
        const auto target = static_cast<long long>(std::llround(analysis + hop_in));
        if (LastCandidate(target, search) + window > end) break;
        analysis += hop_in; ++hops;
        if (ending) break;
    }
    return hops;
}
}
bool TimeStretcher::Configure(core::LinearMemoryArena& arena, unsigned rate, unsigned channels, unsigned capacity) noexcept {
    if (input_ || rate < 8000 || rate > 192000 || !channels || channels > kMaxChannels || arena.IsSealed()) return false;
    hop_ = rate * 12 / 1000; window_ = 2 * hop_; search_ = rate * 8 / 1000;
    if (capacity < 4 * window_ + 2 * search_) return false;
    input_ = static_cast<float*>(arena.Allocate(sizeof(float) * capacity * channels));
    overlap_ = static_cast<float*>(arena.Allocate(sizeof(float) * hop_ * channels));
    out_ = static_cast<float*>(arena.Allocate(sizeof(float) * hop_ * channels));
    window_table_ = static_cast<float*>(arena.Allocate(sizeof(float) * window_));
    region_ = static_cast<float*>(arena.Allocate(sizeof(float) * (2 * search_ + hop_ + 1)));
    natural_ = static_cast<float*>(arena.Allocate(sizeof(float) * hop_));
    if (!input_ || !overlap_ || !out_ || !window_table_ || !region_ || !natural_) { input_ = nullptr; return false; }
    for (unsigned n = 0; n < window_; ++n) window_table_[n] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * n / window_)); // Periodic Hann.
    rate_ = rate; channels_ = channels; capacity_ = capacity;
    Reset();
    return true;
}
void TimeStretcher::Reset() noexcept {
    base_ = read_ = previous_ = 0; held_ = 0; analysis_ = 0; out_count_ = out_read_ = 0; pushed_ = pulled_ = 0;
    passing_ = true; ending_ = false; hop_in_ = hop_;
    change_ = {0, 0, 1};
}
bool TimeStretcher::SetSpeed(float speed) noexcept {
    if (!(speed >= kMinSpeed && speed <= kMaxSpeed)) return false;
    speed_ = speed;
    if (passing_) return true; // Stretching begins at the next Pull with enough input (Begin, in Available/Pull).
    if (speed == 1.0f) { ending_ = true; return true; }
    ending_ = false;
    // From the next hop: its output start stands for the previous frame's analysis position plus a hop.
    hop_in_ = static_cast<double>(speed) * hop_;
    change_ = {pulled_ + (out_count_ - out_read_), analysis_ + hop_, speed};
    return true;
}
unsigned TimeStretcher::Room() const noexcept {
    if (!input_) return 0;
    const auto oldest = (std::max)(base_, Oldest(read_, passing_, previous_, analysis_, hop_in_, hop_, search_));
    const auto used = static_cast<long long>(pushed_) - oldest;
    return used >= static_cast<long long>(capacity_) ? 0u : static_cast<unsigned>(capacity_ - used);
}
void TimeStretcher::Compact() noexcept {
    const auto oldest = (std::max)(base_, Oldest(read_, passing_, previous_, analysis_, hop_in_, hop_, search_));
    if (oldest <= base_) return;
    const auto keep = static_cast<std::size_t>(static_cast<long long>(pushed_) - oldest);
    std::memmove(input_, input_ + (oldest - base_) * channels_, sizeof(float) * keep * channels_);
    base_ = oldest;
}
unsigned TimeStretcher::Push(const float* interleaved, unsigned frames) noexcept {
    frames = (std::min)(frames, Room());
    if (!frames || !interleaved) return 0;
    if (static_cast<long long>(pushed_) + frames - base_ > static_cast<long long>(capacity_)) Compact();
    std::memcpy(input_ + (static_cast<long long>(pushed_) - base_) * channels_, interleaved, sizeof(float) * frames * channels_);
    pushed_ += frames;
    held_ = static_cast<unsigned>(static_cast<long long>(pushed_) - (passing_ ? read_ : previous_ + hop_));
    return frames;
}
float TimeStretcher::Mono(long long index) const noexcept {
    const float* f = Frame(index); float sum = 0;
    for (unsigned c = 0; c < channels_; ++c) sum += f[c];
    return sum / static_cast<float>(channels_);
}
bool TimeStretcher::CanMakeHop() const noexcept {
    const auto target = static_cast<long long>(std::llround(analysis_ + hop_in_));
    return LastCandidate(target, search_) + window_ <= static_cast<long long>(pushed_);
}
long long TimeStretcher::Best(long long target) noexcept {
    // The frame start within +-search of the target whose first half best continues the previous frame's second half
    // (its natural continuation, previous_ + hop_): normalised cross-correlation on the channels' mean, at every 4th
    // offset and sample first, then at every offset around the best one. The means are computed once, into scratch.
    const long long low = (std::max)(target - static_cast<long long>(search_), (std::max)(base_, 0LL));
    const long long high = (std::max)(LastCandidate(target, search_), low); // Never an empty range (low is at the stream's first frame).
    const long long natural = previous_ + hop_;
    const auto span = static_cast<unsigned>(high - low) + hop_; // At most 2 search_ + hop_ + 1 frames.
    for (unsigned n = 0; n < span; ++n) region_[n] = Mono(low + n);
    for (unsigned n = 0; n < hop_; ++n) natural_[n] = Mono(natural + n);
    const auto score = [&](long long start, unsigned step) {
        const float* candidate = region_ + (start - low);
        double dot = 0, energy = 1e-12;
        for (unsigned n = 0; n < hop_; n += step) { const double c = candidate[n]; dot += c * natural_[n]; energy += c * c; }
        return dot / std::sqrt(energy);
    };
    long long best = (std::clamp)(target, low, high); double best_score = score(best, 4);
    for (long long start = low; start <= high; start += 4) { const double s = score(start, 4); if (s > best_score) { best_score = s; best = start; } }
    const long long from = (std::max)(low, best - 3), to = (std::min)(high, best + 3);
    best_score = score(best, 1);
    for (long long start = from; start <= to; ++start) { const double s = score(start, 1); if (s > best_score) { best_score = s; best = start; } }
    return best;
}
void TimeStretcher::MakeHop() noexcept {
    const auto target = static_cast<long long>(std::llround(analysis_ + hop_in_));
    const auto start = Best(target);
    const float* first = Frame(start);
    const float* second = Frame(start + hop_);
    for (unsigned n = 0; n < hop_; ++n) {
        const float rise = window_table_[n], fall = window_table_[hop_ + n];
        for (unsigned c = 0; c < channels_; ++c) {
            out_[n * channels_ + c] = overlap_[n * channels_ + c] + rise * first[n * channels_ + c];
            overlap_[n * channels_ + c] = fall * second[n * channels_ + c];
        }
    }
    out_count_ = hop_; out_read_ = 0;
    previous_ = start; analysis_ += hop_in_;
    if (ending_) {
        // 1x from here: this frame's second half and its natural continuation's first half are the input itself.
        passing_ = true; ending_ = false; read_ = start + hop_; hop_in_ = hop_;
        change_ = {pulled_ + hop_, static_cast<double>(read_), 1.0f};
    }
    held_ = static_cast<unsigned>(static_cast<long long>(pushed_) - (passing_ ? read_ : previous_ + hop_));
}
unsigned TimeStretcher::Available() const noexcept {
    if (!input_) return 0;
    const unsigned made = out_count_ - out_read_;
    const auto end = static_cast<long long>(pushed_);
    if (passing_) {
        const auto copy = static_cast<unsigned>(end - read_);
        if (speed_ == 1.0f) return made + copy;
        // A stretch about to begin (at the next Pull) needs a hop of input to start from.
        if (copy < hop_) return made;
        return made + hop_ * Hops(static_cast<double>(read_ - hop_), static_cast<double>(speed_) * hop_, end, search_, window_, false);
    }
    // Whole hops that the input already allows (a lower bound: an ending stretch passes on more).
    return made + hop_ * Hops(analysis_, hop_in_, end, search_, window_, ending_);
}
unsigned TimeStretcher::Pull(float* interleaved, unsigned frames) noexcept {
    unsigned done = 0;
    while (done < frames) {
        if (out_read_ < out_count_) {
            const unsigned n = (std::min)(frames - done, out_count_ - out_read_);
            std::memcpy(interleaved + static_cast<std::size_t>(done) * channels_, out_ + static_cast<std::size_t>(out_read_) * channels_, sizeof(float) * n * channels_);
            out_read_ += n; done += n; pulled_ += n;
            continue;
        }
        if (passing_ && speed_ != 1.0f) {
            // Begin stretching here: the frame before the first one started a hop back, and its second half (the next
            // hop of input, windowed) is the overlap.
            if (static_cast<long long>(pushed_) - read_ < static_cast<long long>(hop_)) break;
            passing_ = false; hop_in_ = static_cast<double>(speed_) * hop_;
            previous_ = read_ - hop_; analysis_ = static_cast<double>(previous_);
            const float* half = Frame(read_);
            for (unsigned n = 0; n < hop_; ++n)
                for (unsigned c = 0; c < channels_; ++c) overlap_[n * channels_ + c] = window_table_[hop_ + n] * half[n * channels_ + c];
            change_ = {pulled_, static_cast<double>(read_), speed_};
            continue;
        }
        if (passing_) {
            const unsigned n = static_cast<unsigned>((std::min)(static_cast<long long>(frames - done), static_cast<long long>(pushed_) - read_));
            if (!n) break;
            std::memcpy(interleaved + static_cast<std::size_t>(done) * channels_, Frame(read_), sizeof(float) * n * channels_);
            read_ += n; done += n; pulled_ += n;
            continue;
        }
        if (!CanMakeHop()) break;
        MakeHop();
    }
    held_ = static_cast<unsigned>(static_cast<long long>(pushed_) - (passing_ ? read_ : previous_ + hop_));
    return done;
}
}
