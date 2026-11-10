#include "media/clock/PredictiveMasterClock.hpp"
#include "media/audio/PcmRingBuffer.hpp"
#include "media/video/SurfaceLeasePool.hpp"
#include "media/video/ReorderWindow.hpp"
#include "allocation_probe.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 30) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
platform::AudioClockSample Sample(std::uint64_t position, std::int64_t time) {
    return {position, 10000000, time, true, 1};
}
void ClockConvergence() {
    for (const double rate : {0.9995, 1.0005}) {
        clock::PredictiveMasterClock pll;
        CHECK(pll.Reanchor(Sample(0, 10000000), -5000000, 7));
        allocation_probe::Begin();
        for (std::int64_t i = 1; i <= 6000; ++i) {
            // Deterministic bounded 40us timestamp jitter and 500ppm oscillator skew.
            const auto jitter = (i % 5 - 2) * 200;
            auto sample = Sample(static_cast<std::uint64_t>(std::llround(static_cast<double>(i * 100000) * rate)),
                                 10000000 + i * 100000 + jitter);
            CHECK(pll.Observe(sample) == clock::ClockUpdate::Accepted);
            if (i > 500) {
                clock::ClockEstimate estimate;
                CHECK(pll.Estimate(10001000 + i * 100000, estimate) == clock::ClockQuery::Ready);
                const auto actual = -5000000 + static_cast<std::int64_t>(std::llround(static_cast<double>(i * 100000 + 1000) * rate));
                CHECK(std::abs(estimate.media_time_100ns - actual) < 1000); // 100us, stronger than 1ms.
                CHECK(std::abs(estimate.rate - rate) < 0.00002);
                CHECK(estimate.epoch == 7);
                std::int64_t deadline = 0; std::uint32_t epoch = 0;
                CHECK(pll.Deadline(actual + 333333, 10001000 + i * 100000, deadline, epoch) == clock::ClockQuery::Ready);
                const auto target = 10001000 + i * 100000 + static_cast<std::int64_t>(std::llround(333333.0 / rate));
                CHECK(std::abs(deadline - target) < 1000 && epoch == 7);
            }
        }
        CHECK(allocation_probe::End() == 0);
    }
}
// Playback speed (docs/audio-dsp.md, "Playback speed"): with the device running 500 ppm fast and jittered as above, media
// time advances `speed` times as fast as device time, and a picture's deadline is its media distance divided by both.
void ClockSpeed() {
    for (const double speed : {0.25, 0.5, 1.5, 2.0}) {
        clock::PredictiveMasterClock pll;
        CHECK(pll.Reanchor(Sample(0, 10000000), 30000000, 3, speed));
        constexpr double rate = 1.0005;
        for (std::int64_t i = 1; i <= 3000; ++i) {
            const auto jitter = (i % 5 - 2) * 200;
            CHECK(pll.Observe(Sample(static_cast<std::uint64_t>(std::llround(static_cast<double>(i * 100000) * rate)), 10000000 + i * 100000 + jitter)) ==
                  clock::ClockUpdate::Accepted);
            if (i <= 500) continue;
            clock::ClockEstimate estimate;
            CHECK(pll.Estimate(10001000 + i * 100000, estimate) == clock::ClockQuery::Ready && estimate.speed == speed);
            const auto actual = 30000000 + static_cast<std::int64_t>(std::llround(speed * static_cast<double>(i * 100000 + 1000) * rate));
            CHECK(std::abs(estimate.media_time_100ns - actual) < 1000 * speed + 1);
            std::int64_t deadline = 0; std::uint32_t epoch = 0;
            CHECK(pll.Deadline(actual + 333333, 10001000 + i * 100000, deadline, epoch) == clock::ClockQuery::Ready);
            CHECK(std::abs(deadline - (10001000 + i * 100000 + static_cast<std::int64_t>(std::llround(333333.0 / speed / rate)))) < 1000);
        }
    }
    clock::PredictiveMasterClock pll;
    CHECK(!pll.Reanchor(Sample(0, 0), 0, 1, 0.2) && !pll.Reanchor(Sample(0, 0), 0, 1, 4.5) && !pll.Reanchor(Sample(0, 0), 0, 1, std::nan("")));
    CHECK(pll.Reanchor(Sample(0, 0), 0, 1, 0.25) && pll.Reanchor(Sample(0, 0), 0, 1, 4.0));
}
void ClockRejections() {
    clock::PredictiveMasterClock pll;
    clock::ClockEstimate estimate;
    CHECK(pll.Estimate(0, estimate) == clock::ClockQuery::NotAnchored);
    CHECK(!pll.Reanchor({0, 0, 0, true, 1}, 0, 1));
    CHECK(pll.Reanchor(Sample(0, 0), 0, 1));
    CHECK(pll.Observe(Sample(100, 100)) == clock::ClockUpdate::TooSoon);
    auto imprecise = Sample(100000, 100000); imprecise.precise = false;
    CHECK(pll.Observe(imprecise) == clock::ClockUpdate::Rejected);
    CHECK(pll.Observe(imprecise) == clock::ClockUpdate::Rejected);
    CHECK(pll.Observe(imprecise) == clock::ClockUpdate::NeedsAnchor);
    CHECK(pll.Estimate(100000, estimate) == clock::ClockQuery::NotAnchored);
    CHECK(pll.Reanchor(Sample(0, 0), 0, 2));
    CHECK(pll.Observe(Sample(0, 100000)) == clock::ClockUpdate::Rejected); // Stutter.
    CHECK(pll.Observe(Sample(100000, 100000)) == clock::ClockUpdate::Accepted);
    CHECK(pll.Observe(Sample(10000000, 200000)) == clock::ClockUpdate::Rejected); // Phase jump.
    auto changed = Sample(200000, 200000); changed.generation = 2;
    CHECK(pll.Observe(changed) == clock::ClockUpdate::NeedsAnchor);
    CHECK(pll.Reanchor(Sample(0, 0), 0, 3));
    changed = Sample(100000, 100000); changed.frequency = 48000;
    CHECK(pll.Observe(changed) == clock::ClockUpdate::NeedsAnchor);
    CHECK(pll.Reanchor(Sample(0, 0), 0, 4));
    CHECK(pll.Observe(Sample(10000000, 10000000)) == clock::ClockUpdate::NeedsAnchor); // Sleep.
    CHECK(pll.Reanchor(Sample(100000, 100000), 0, 5));
    CHECK(pll.Observe(Sample(99999, 200000)) == clock::ClockUpdate::NeedsAnchor);
    CHECK(pll.Reanchor(Sample(0, 100000), 0, 6));
    CHECK(pll.Estimate(3000000, estimate) == clock::ClockQuery::Stale);
    std::int64_t deadline = 0; std::uint32_t epoch = 0;
    CHECK(pll.Deadline(INT64_MAX, 100000, deadline, epoch) == clock::ClockQuery::OutOfRange);
    CHECK(pll.Reanchor(Sample(0, 0), INT64_MAX - 10, 7));
    CHECK(pll.Estimate(100000, estimate) == clock::ClockQuery::OutOfRange);
    CHECK(!pll.Reanchor(Sample(0, 0), INT64_MIN, 8));
    clock::ClockOptions bad; bad.time_constant_seconds = 0.0;
    clock::PredictiveMasterClock invalid(bad);
    CHECK(!invalid.Reanchor(Sample(0, 0), 0, 1));
}
void ClockConcurrency() {
    clock::PredictiveMasterClock pll;
    CHECK(pll.Reanchor(Sample(0, 1000000), 0, 0));
    std::atomic<unsigned> ready{0}, done{0}, errors{0};
    std::atomic<bool> start{false}, quit{false};
    auto barrier = [&] { ready.fetch_add(1); while (!start.load()) std::this_thread::yield(); };
    std::thread writer([&] {
        barrier();
        for (std::uint32_t i = 1; i <= 100000; ++i)
            if (!pll.Reanchor(Sample(0, 1000000), static_cast<std::int64_t>(i) * 1000, i)) errors.fetch_add(1);
        done.fetch_add(1); while (!quit.load()) std::this_thread::yield();
    });
    auto reader = [&] {
        barrier();
        for (unsigned i = 0; i != 100000; ++i) {
            clock::ClockEstimate value;
            const auto status = pll.Estimate(1000000, value);
            if (status != clock::ClockQuery::Busy && (status != clock::ClockQuery::Ready ||
                value.media_time_100ns != static_cast<std::int64_t>(value.epoch) * 1000 || value.rate != 1.0)) errors.fetch_add(1);
        }
        done.fetch_add(1); while (!quit.load()) std::this_thread::yield();
    };
    std::thread a(reader), b(reader);
    while (ready.load() != 3) std::this_thread::yield();
    allocation_probe::Begin(); start.store(true);
    while (done.load() != 3) std::this_thread::yield();
    CHECK(allocation_probe::End() == 0);
    quit.store(true); writer.join(); a.join(); b.join();
    CHECK(errors.load() == 0);
}
void PcmBounds() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4096));
    audio::PcmRingBuffer ring;
    CHECK(!ring.Initialize(arena, 7, 4)); CHECK(ring.Initialize(arena, 8, 4)); CHECK(arena.Seal());
    platform::AudioWriteSpan span{}, same{};
    std::array<std::uint32_t, 8> out{};
    auto bytes = std::as_writable_bytes(std::span(out));
    allocation_probe::Begin();
    CHECK(ring.ReadInto(bytes) == 0);
    CHECK(ring.TryAcquire(0, span) == platform::EndpointStatus::InvalidArgument);
    CHECK(ring.TryAcquire(8, span) == platform::EndpointStatus::Ready && span.frames == 8);
    CHECK(ring.TryAcquire(2, same) == platform::EndpointStatus::Ready && same.lease == span.lease && same.frames == 8);
    CHECK(ring.Commit(span.lease + 1, 8) == platform::EndpointStatus::InvalidArgument);
    CHECK(ring.Commit(span.lease, 9) == platform::EndpointStatus::InvalidArgument);
    for (std::uint32_t i = 0; i != 8; ++i) std::memcpy(span.data + i * 4, &i, 4);
    CHECK(ring.Commit(span.lease, 8) == platform::EndpointStatus::Ready);
    CHECK(ring.Commit(span.lease, 8) == platform::EndpointStatus::InvalidArgument);
    CHECK(ring.TryAcquire(1, span) == platform::EndpointStatus::WouldBlock);
    CHECK(ring.ReadInto(bytes.first(12)) == 3 && out[0] == 0 && out[2] == 2);
    CHECK(ring.TryAcquire(8, span) == platform::EndpointStatus::Ready && span.frames == 3);
    for (std::uint32_t i = 8; i != 11; ++i) std::memcpy(span.data + (i - 8) * 4, &i, 4);
    CHECK(ring.Commit(span.lease, 3) == platform::EndpointStatus::Ready);
    CHECK(ring.ReadInto(bytes.first(3)) == 0); // Partial frame prohibited.
    CHECK(ring.ReadInto(bytes) == 8);
    for (std::uint32_t i = 0; i != 8; ++i) CHECK(out[i] == i + 3);
    CHECK(ring.TryAcquire(8, span) == platform::EndpointStatus::Ready && span.frames == 5);
    CHECK(ring.Commit(span.lease, 0) == platform::EndpointStatus::Ready);
    CHECK(ring.QueuedFrames() == 0 && ring.ReadInto(bytes) == 0);
    // Seek flush: the consumer discards queued frames, the producer regains the whole capacity, and later
    // frames read back intact.
    CHECK(ring.TryAcquire(6, span) == platform::EndpointStatus::Ready && span.frames == 5);
    CHECK(ring.Commit(span.lease, 5) == platform::EndpointStatus::Ready && ring.QueuedFrames() == 5);
    CHECK(ring.Discard() == 5 && ring.QueuedFrames() == 0 && ring.Discard() == 0 && ring.ReadInto(bytes) == 0);
    CHECK(ring.TryAcquire(8, span) == platform::EndpointStatus::Ready && span.frames == 8);
    for (std::uint32_t i = 0; i != 8; ++i) std::memcpy(span.data + i * 4, &i, 4);
    CHECK(ring.Commit(span.lease, 8) == platform::EndpointStatus::Ready);
    CHECK(ring.ReadInto(bytes) == 8 && out[0] == 0 && out[7] == 7);
    // Null endpoint: frames consumed unread, bounded by what is queued; the next read continues after them.
    CHECK(ring.TryAcquire(6, span) == platform::EndpointStatus::Ready && span.frames == 6);
    for (std::uint32_t i = 0; i != 6; ++i) std::memcpy(span.data + i * 4, &i, 4);
    CHECK(ring.Commit(span.lease, 6) == platform::EndpointStatus::Ready);
    CHECK(ring.Skip(2) == 2 && ring.QueuedFrames() == 4 && ring.ReadInto(bytes.first(4)) == 1 && out[0] == 2);
    CHECK(ring.Skip(10) == 3 && ring.QueuedFrames() == 0 && ring.Skip(1) == 0);
    CHECK(allocation_probe::End() == 0);
}
void PcmConcurrency() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(65536));
    audio::PcmRingBuffer ring; CHECK(ring.Initialize(arena, 1024, 8)); CHECK(arena.Seal());
    constexpr std::uint64_t count = 500000;
    std::atomic<unsigned> ready{0}, done{0}, errors{0};
    std::atomic<bool> start{false}, quit{false};
    auto barrier = [&] { ready.fetch_add(1); while (!start.load()) std::this_thread::yield(); };
    std::thread producer([&] {
        barrier(); std::uint64_t written = 0;
        while (written != count) {
            platform::AudioWriteSpan span{};
            const auto status = ring.TryAcquire(static_cast<std::uint32_t>((count - written < 31) ? count - written : 31), span);
            if (status == platform::EndpointStatus::WouldBlock) continue;
            if (status != platform::EndpointStatus::Ready) { errors.fetch_add(1); break; }
            for (std::uint32_t i = 0; i != span.frames; ++i) { const auto value = written + i; std::memcpy(span.data + i * 8, &value, 8); }
            if (ring.Commit(span.lease, span.frames) != platform::EndpointStatus::Ready) { errors.fetch_add(1); break; }
            written += span.frames;
        }
        done.fetch_add(1); while (!quit.load()) std::this_thread::yield();
    });
    std::thread consumer([&] {
        barrier(); std::uint64_t read = 0; std::array<std::uint64_t, 47> values{};
        while (read != count) {
            const auto frames = ring.ReadInto(std::as_writable_bytes(std::span(values)));
            for (std::uint32_t i = 0; i != frames; ++i) if (values[i] != read + i) errors.fetch_add(1);
            read += frames;
        }
        done.fetch_add(1); while (!quit.load()) std::this_thread::yield();
    });
    while (ready.load() != 2) std::this_thread::yield();
    allocation_probe::Begin(); start.store(true);
    while (done.load() != 2) std::this_thread::yield();
    CHECK(allocation_probe::End() == 0);
    quit.store(true); producer.join(); consumer.join(); CHECK(errors.load() == 0);
}
void SurfaceLifetime() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4096));
    video::SurfaceLeasePool pool; CHECK(pool.Initialize(arena, 1)); CHECK(arena.Seal());
    platform::SurfaceHandle a{}, b{}; std::uint64_t wait = 0;
    allocation_probe::Begin();
    CHECK(pool.Reserve(0, 0, a)); CHECK(!pool.Reserve(0, 0, b));
    CHECK(pool.Cancel(a)); CHECK(!pool.Valid(a)); CHECK(pool.Reserve(0, 0, a));
    CHECK(pool.Submitted(a, 1, true)); CHECK(!pool.Cancel(a));
    CHECK(!pool.AllConsumersReturned());
    CHECK(!pool.ReferenceReady(a, wait)); CHECK(!pool.Reserve(100, 100, b));
    CHECK(pool.Return(a, 3)); CHECK(!pool.Return(a, 3));
    CHECK(pool.AllConsumersReturned());
    CHECK(pool.ReferenceReady(a, wait) && wait == 3);
    CHECK(pool.ReferenceUsed(a, 5)); CHECK(!pool.ReferenceUsed(a, 4));
    CHECK(!pool.Reserve(100, 100, b)); // DPB hold still active.
    CHECK(pool.ReleaseReference(a)); CHECK(!pool.ReleaseReference(a));
    CHECK(!pool.Reserve(4, 3, b)); CHECK(!pool.Reserve(5, 2, b));
    CHECK(!pool.Reserve(UINT64_MAX, 100, b)); // Removed device cannot look complete.
    CHECK(pool.Reserve(5, 3, b) && b.id == a.id && b.generation != a.generation);
    CHECK(!pool.Valid(a)); CHECK(!pool.Return(a, 0));
    CHECK(pool.Submitted(b, 6, false)); CHECK(pool.Return(b, 0));
    CHECK(!pool.Reserve(5, 3, a)); CHECK(pool.Reserve(6, 3, a));
    // Device loss: a submitted, unreturned reference lease is voided. The slot is free at once (fence values of the
    // lost device mean nothing), and the old handle never validates again.
    CHECK(pool.Submitted(a, 7, true)); CHECK(!pool.Reserve(UINT64_MAX - 1, UINT64_MAX - 1, b));
    pool.Reset();
    CHECK(!pool.Valid(a) && !pool.Return(a, 1) && !pool.ReleaseReference(a) && pool.AllConsumersReturned());
    CHECK(pool.Reserve(0, 0, b) && b.id == a.id && b.generation != a.generation);
    // A reference no consumer reads (a picture decoded but not displayed) predicts the next picture at once, with no
    // consumer fence to wait for; its slot is still not reused before its return arrives and its decode completed.
    CHECK(pool.Submitted(b, 8, true, true));
    CHECK(pool.ReferenceReady(b, wait) && wait == 0);
    CHECK(pool.ReleaseReference(b) && !pool.Reserve(100, 100, a)); // Not returned yet: not free.
    CHECK(pool.Return(b, 0) && !pool.Reserve(7, 0, a) && pool.Reserve(8, 0, a));
    CHECK(pool.Submitted(a, 9, true, false) && !pool.ReferenceReady(a, wait)); // A displayed reference still waits for its return.
    CHECK(pool.Return(a, 12) && pool.ReferenceReady(a, wait) && wait == 12);
    CHECK(allocation_probe::End() == 0);
}
// Decode-order PTS sequences from hierarchical GOPs of varying size, decoded as: anchor first, then a
// recursive bisection of the preceding interval (the random-access structure of H.265 encoders).
unsigned GopOrder(unsigned gop, unsigned count, std::int64_t* out) {
    unsigned n = 0; out[n++] = 0;
    struct Range { std::int64_t lo, hi; } stack[64]{};
    for (std::int64_t base = 0; n + gop <= count; base += gop) {
        out[n++] = base + gop;
        unsigned depth = 0; stack[depth++] = {base, base + gop};
        while (depth) {
            const auto r = stack[--depth];
            if (r.hi - r.lo < 2) continue;
            const auto mid = (r.lo + r.hi) / 2; out[n++] = mid;
            stack[depth++] = {mid, r.hi}; stack[depth++] = {r.lo, mid}; // Lower half decodes first.
        }
    }
    return n;
}
std::uint32_t ExactDepth(const std::int64_t* pts, unsigned count) {
    std::uint32_t depth = 0;
    for (unsigned i = 0; i < count; ++i) {
        std::uint32_t later = 0; for (unsigned j = 0; j < i; ++j) later += pts[j] > pts[i] ? 1u : 0u;
        depth = later > depth ? later : depth;
    }
    return depth;
}
// Feeds a decode-order sequence through the window with bumping; returns false on any Late/ordering error.
bool Run(video::ReorderWindow<16>& window, const std::int64_t* pts, unsigned count, unsigned& released) {
    std::int64_t last = INT64_MIN; released = 0; bool ordered = true;
    auto drain = [&](bool draining) {
        while (window.Ready(draining)) {
            const auto* front = window.Front();
            ordered = ordered && front->pts > last; last = front->pts;
            (void)window.Pop(); ++released;
        }
    };
    for (unsigned i = 0; i < count; ++i) {
        if (window.Insert(pts[i], i) != video::ReorderInsert::Inserted) return false;
        drain(false);
    }
    drain(true);
    return ordered;
}
void Reorder() {
    video::ReorderWindow<16> window;
    CHECK(!window.Configure(15)); CHECK(window.Configure(14)); CHECK(window.Configure(0));
    std::int64_t pts[64]{};
    unsigned released = 0, too_shallow_detected = 0;
    allocation_probe::Begin();
    for (const unsigned gop : {1u, 2u, 4u, 8u, 16u}) {
        const auto count = GopOrder(gop, 49, pts);
        const auto depth = ExactDepth(pts, count);
        CHECK(window.Configure(depth));
        CHECK(Run(window, pts, count, released) && released == count && window.Size() == 0);
        if (depth) {
            video::ReorderWindow<16> shallow; CHECK(shallow.Configure(depth - 1));
            too_shallow_detected += Run(shallow, pts, count, released) ? 0u : 1u;
        }
    }
    CHECK(too_shallow_detected == 4); // One less than the exact depth always releases a frame too early.
    CHECK(GopOrder(4, 9, pts) == 9 && pts[1] == 4 && pts[2] == 2 && pts[3] == 1 && pts[4] == 3 && ExactDepth(pts, 9) == 2);
    // Duplicates, capacity and late arrivals are reported, never silently reordered.
    video::ReorderWindow<4> small; CHECK(small.Configure(2));
    CHECK(small.Insert(30, 0) == video::ReorderInsert::Inserted); CHECK(small.Insert(10, 1) == video::ReorderInsert::Inserted);
    CHECK(small.Insert(10, 2) == video::ReorderInsert::Duplicate); CHECK(!small.Ready(false)); CHECK(small.Ready(true));
    CHECK(small.Insert(20, 2) == video::ReorderInsert::Inserted); CHECK(small.Ready(false));
    CHECK(small.Front()->pts == 10 && small.Front()->slot == 1); CHECK(small.Pop());
    CHECK(small.Insert(5, 3) == video::ReorderInsert::Late);
    CHECK(small.Insert(40, 3) == video::ReorderInsert::Inserted); CHECK(small.Insert(50, 0) == video::ReorderInsert::Inserted);
    CHECK(small.Insert(60, 1) == video::ReorderInsert::Full);
    CHECK(small.Front()->pts == 20 && small.Pop() && small.Front()->pts == 30 && small.Pop() && small.Pop() && small.Pop());
    CHECK(!small.Pop() && !small.Front() && small.Configure(0));
    CHECK(allocation_probe::End() == 0);
}
}
int main() {
    ClockConvergence(); ClockSpeed(); ClockRejections(); ClockConcurrency(); PcmBounds(); PcmConcurrency(); SurfaceLifetime(); Reorder();
    std::printf("Layer 2 portable tests: %d failures\n", failures); return failures ? 1 : 0;
}
