#pragma once

#include "media/core/platform.hpp"

#include <atomic>
#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Cache-line padding of the cursor is intentional.
#endif

namespace media::core {

// What the engine did, as one flight recorder entry (docs/session.md, "Flight recorder"). The meaning of the fields
// depends on the kind; every time is 100 ns on the engine's clock (Now()).
enum class FlightEvent : std::uint16_t {
    None,
    SeekRequested,  // a: 1 scrub, 2 queue seek; b: epoch; c: target.
    SeekLanded,     // b: epoch; c: PTS shown; d: request-to-screen latency.
    SeekApplied,    // Worker. a: bit 0 continued, bit 1 queue seek, bit 2 preview; b: epoch; c: target.
    TierChange,     // a: from << 8 | to (render::QualityTier); b: reason (QualityGovernor::Reason); c: p95; d: budget.
    Drop,           // a: what it waited for (D3D12Renderer::Wait mask); b: 1 behind the clock; c: PTS; d: lateness.
    Pause,          // c: media position.
    Resume,         // c: media position.
    Speed,          // b: speed in thousandths.
    Zoom,           // b: zoom in hundredths; c: centre x, d: centre y (millionths of the crop).
    TrackRequested, // a: 1 audio, 2 subtitles; b: track id.
    TrackSwitched,  // Worker. a: 1 spliced; b: track id; c: media time of the splice; d: request-to-splice time.
    AudioMoved,     // Worker. The endpoint changed (default device, loss): b: changes so far.
    Underrun,       // Worker. The clock re-anchored after the endpoint ran dry: b: re-anchors so far.
    DeviceLost,     // a: detector (1 presentation, 2 worker); b: HRESULT.
    DeviceRecovered,// c: loss-to-rebuilt time.
    LoopSet,        // c: A; d: B (media time). a: 1 from a subtitle cue.
    LoopCleared,
    LoopWrap,       // a: 1 through the pinned pictures; b: wraps so far; c: B (where it turned); d: A.
    Stats,          // The telemetry readout was asked for. a: 1 the quality explanation.
    NightMode,      // a: 1 on, 0 off.
    Bookmark,       // a: 1 added, 2 gone to, 3 frame pinned; b: its number (from 1); c: its media time.
    Count
};
struct FlightRecord {
    std::int64_t time = 0;
    FlightEvent kind = FlightEvent::None;
    std::uint16_t a = 0;
    std::uint32_t b = 0;
    std::int64_t c = 0, d = 0;
};
static_assert(sizeof(FlightRecord) == 32);

// A bounded flight recorder: the newest `Capacity` records of one writer thread, readable at any time from another
// thread without stopping the writer. Fixed storage (no allocation), no locks, no waits: Record is a constant number of
// relaxed stores and two release stores; a reader takes the newest records it can read whole. Each slot is a sequence
// lock: the writer marks it odd while it writes and stamps it with the record's number when done, so a reader drops a
// record the writer overwrote while it was being read instead of returning a mixture of two.
// Exactly one writer. Readers: any number, on any thread.
template <unsigned Capacity>
class FlightRecorder final {
    static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a nonzero power of two.");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    struct Slot {
        std::atomic<std::uint64_t> sequence{0}; // 2n + 2 once record n is whole in it; odd while it is being written.
        std::atomic<std::uint64_t> words[4]{};
    };
public:
    static constexpr unsigned kCapacity = Capacity;
    FlightRecorder() noexcept = default;
    FlightRecorder(const FlightRecorder&) = delete;
    FlightRecorder& operator=(const FlightRecorder&) = delete;
    // Writer only.
    void Record(const FlightRecord& r) noexcept {
        const std::uint64_t n = written_.value.load(std::memory_order_relaxed);
        Slot& slot = slots_[n % Capacity];
        slot.sequence.store(2 * n + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release); // The odd mark is seen before any new word.
        slot.words[0].store(static_cast<std::uint64_t>(r.time), std::memory_order_relaxed);
        slot.words[1].store(static_cast<std::uint64_t>(r.kind) | std::uint64_t{r.a} << 16 | std::uint64_t{r.b} << 32, std::memory_order_relaxed);
        slot.words[2].store(static_cast<std::uint64_t>(r.c), std::memory_order_relaxed);
        slot.words[3].store(static_cast<std::uint64_t>(r.d), std::memory_order_relaxed);
        slot.sequence.store(2 * n + 2, std::memory_order_release);
        written_.value.store(n + 1, std::memory_order_release);
    }
    void Record(std::int64_t time, FlightEvent kind, std::uint16_t a = 0, std::uint32_t b = 0, std::int64_t c = 0, std::int64_t d = 0) noexcept {
        Record(FlightRecord{time, kind, a, b, c, d});
    }
    // Records made so far (also those overwritten since).
    std::uint64_t Written() const noexcept { return written_.value.load(std::memory_order_acquire); }
    // The newest records, oldest first, at most `max` (and at most Capacity): those read whole. Returns how many.
    // `first` (optional) receives the number of the first one returned (records are numbered from 0 in writing order).
    unsigned Snapshot(FlightRecord* records, unsigned max, std::uint64_t* first = nullptr) const noexcept {
        if (!records || !max) return 0;
        const std::uint64_t end = written_.value.load(std::memory_order_acquire);
        const std::uint64_t span = max < Capacity ? max : Capacity;
        const std::uint64_t begin = end > span ? end - span : 0;
        unsigned count = 0; bool numbered = false;
        for (std::uint64_t n = begin; n < end; ++n) {
            const Slot& slot = slots_[n % Capacity];
            const std::uint64_t before = slot.sequence.load(std::memory_order_acquire);
            if (before != 2 * n + 2) continue; // Overwritten by a newer record (or being written).
            std::uint64_t w[4];
            for (unsigned i = 0; i < 4; ++i) w[i] = slot.words[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire); // The words are read before the second look.
            if (slot.sequence.load(std::memory_order_relaxed) != before) continue;
            FlightRecord& r = records[count++];
            r.time = static_cast<std::int64_t>(w[0]); r.kind = static_cast<FlightEvent>(w[1] & 0xffff);
            r.a = static_cast<std::uint16_t>(w[1] >> 16); r.b = static_cast<std::uint32_t>(w[1] >> 32);
            r.c = static_cast<std::int64_t>(w[2]); r.d = static_cast<std::int64_t>(w[3]);
            if (!numbered && first) { *first = n; numbered = true; }
        }
        return count;
    }
    // The newest record of `kind` among the last Capacity, read whole. False: none.
    bool Last(FlightEvent kind, FlightRecord& found) const noexcept {
        const std::uint64_t end = written_.value.load(std::memory_order_acquire);
        for (std::uint64_t k = 0; k < Capacity && k < end; ++k) {
            const std::uint64_t n = end - 1 - k;
            const Slot& slot = slots_[n % Capacity];
            const std::uint64_t before = slot.sequence.load(std::memory_order_acquire);
            if (before != 2 * n + 2) continue;
            std::uint64_t w[4];
            for (unsigned i = 0; i < 4; ++i) w[i] = slot.words[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.sequence.load(std::memory_order_relaxed) != before || static_cast<FlightEvent>(w[1] & 0xffff) != kind) continue;
            found.time = static_cast<std::int64_t>(w[0]); found.kind = kind;
            found.a = static_cast<std::uint16_t>(w[1] >> 16); found.b = static_cast<std::uint32_t>(w[1] >> 32);
            found.c = static_cast<std::int64_t>(w[2]); found.d = static_cast<std::int64_t>(w[3]);
            return true;
        }
        return false;
    }
private:
    struct alignas(kCacheLineBytes) Cursor { std::atomic<std::uint64_t> value{0}; };
    Cursor written_;                       // Its own cache line: the reader's polls never share the slots' lines with it.
    alignas(kCacheLineBytes) Slot slots_[Capacity];
};
// The engine's two recorders (the presentation thread's and the media worker's), merged in time order: at most `max`
// of the newest records of both, oldest first. Returns how many.
template <unsigned A, unsigned B>
unsigned MergeNewest(const FlightRecorder<A>& first, const FlightRecorder<B>& second, FlightRecord* merged, unsigned max,
                     FlightRecord* scratch_a, FlightRecord* scratch_b) noexcept {
    const unsigned na = first.Snapshot(scratch_a, max), nb = second.Snapshot(scratch_b, max);
    // Merge from the newest end so that the `max` newest of both survive.
    unsigned ia = na, ib = nb, total = na + nb < max ? na + nb : max, k = total;
    while (k) {
        const bool take_a = ib == 0 || (ia != 0 && scratch_a[ia - 1].time >= scratch_b[ib - 1].time);
        merged[--k] = take_a ? scratch_a[--ia] : scratch_b[--ib];
    }
    return total;
}
inline const char* Name(FlightEvent kind) noexcept {
    static constexpr const char* names[]{"None", "SeekRequested", "SeekLanded", "SeekApplied", "TierChange", "Drop", "Pause", "Resume", "Speed",
                                         "Zoom", "TrackRequested", "TrackSwitched", "AudioMoved", "Underrun", "DeviceLost", "DeviceRecovered",
                                         "LoopSet", "LoopCleared", "LoopWrap", "Stats", "NightMode", "Bookmark"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<unsigned>(FlightEvent::Count));
    return static_cast<unsigned>(kind) < static_cast<unsigned>(FlightEvent::Count) ? names[static_cast<unsigned>(kind)] : "?";
}

}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
