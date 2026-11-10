// media::render::QualityGovernor (docs/picture.md, "Quality governor"): the rolling p95, the step-down and step-up rules,
// hysteresis (no oscillation between tiers), exponential back-off, memory of a tier's cost, manual modes, neural
// availability, workload changes. A simulated 24 fps stream; no GPU. No allocation.
// 2026-11-03: the thin-queue rule (docs/fixtures.md, "Cold starts"): while less than 100 ms is converted ahead, one picture
// over half the interval steps down at once (a start at 55 %: down on its first picture, counted as thin); the same picture
// with a full queue changes nothing; Edge-directed comes back once the queue is full, its back-off has passed and Fast has
// shown the headroom; an unknown queue (-1) leaves the rule out.
#include "media/render/QualityGovernor.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdint>

using namespace media::render;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr std::int64_t kBudget = 416667; // 24 fps.
constexpr std::uint64_t kWorkload = 1280ull * 720 << 32 | 640 * 360;
struct Stream {
    QualityGovernor governor;
    std::int64_t now = 1000000000; // 100 s: any epoch.
    unsigned changes = 0;
    std::int64_t queued = -1; // The media converted ahead (the thin-queue rule; -1: unknown).
    // One picture at the governor's tier costing `fraction` of the budget (or `as`: converted at another tier).
    bool Picture(double fraction, std::uint64_t workload = kWorkload) {
        now += kBudget;
        const bool changed = governor.Observe(governor.Tier(), static_cast<std::int64_t>(fraction * kBudget), workload, kBudget, now, queued);
        changes += changed ? 1u : 0u;
        return changed;
    }
    // Pictures until the tier changes (at most `limit`): how many it took (limit + 1: no change).
    unsigned Until(double fraction, unsigned limit) { for (unsigned i = 1; i <= limit; ++i) if (Picture(fraction)) return i; return limit + 1; }
};
void Percentile() {
    RollingPercentile p;
    CHECK(p.Count() == 0 && p.P95() == 0);
    std::uint32_t seed = 12345;
    std::int64_t all[200]{};
    for (unsigned n = 1; n <= 200; ++n) {
        seed = seed * 1664525u + 1013904223u;
        const auto v = static_cast<std::int64_t>(seed >> 8) % 100000 - 1000; // Negative values too.
        all[n - 1] = v; p.Add(v);
        // Reference: the last min(n, 32) values sorted, nearest rank ceil(0.95 k).
        const unsigned k = n < RollingPercentile::kWindow ? n : RollingPercentile::kWindow;
        std::int64_t window[RollingPercentile::kWindow]{};
        for (unsigned i = 0; i < k; ++i) window[i] = all[n - k + i];
        std::sort(window, window + k);
        const unsigned rank = (k * 95 + 99) / 100;
        CHECK(p.Count() == k && p.P95() == window[rank - 1] && p.Last() == v);
    }
    // 32 values: the second largest (rank 31); 8 values: the largest.
    p.Clear(); for (int i = 1; i <= 32; ++i) p.Add(i); CHECK(p.P95() == 31);
    p.Clear(); for (int i = 1; i <= 8; ++i) p.Add(i); CHECK(p.P95() == 8);
}
void StepUpAndDown() {
    Stream s; s.governor.Configure();
    CHECK(s.governor.Mode() == QualityMode::Auto && s.governor.Tier() == QualityTier::EdgeDirected);
    s.governor.SetNeuralAvailable(true, s.now);
    // Light edge-directed load (20 %): Neural is tried once a full window (32) and 1 s have passed: picture 32 (1.33 s).
    CHECK(s.Until(0.2, 100) == 32 && s.governor.Tier() == QualityTier::Neural);
    CHECK(s.governor.LastChange().reason == QualityGovernor::Reason::Headroom && s.governor.LastChange().from == QualityTier::EdgeDirected);
    // Pictures converted before the change are not Neural's: ignored.
    CHECK(!s.governor.Observe(QualityTier::EdgeDirected, kBudget, kWorkload, kBudget, s.now + 1) && s.governor.Stats().ignored == 1);
    // Neural at 90 %: down after 20 pictures (the p95 rule), and Neural's cost is remembered.
    CHECK(s.Until(0.9, 100) == 20 && s.governor.Tier() == QualityTier::EdgeDirected);
    CHECK(s.governor.LastChange().reason == QualityGovernor::Reason::Overload && s.governor.Known(QualityTier::Neural) == static_cast<std::int64_t>(0.9 * kBudget));
    // Edge-directed light again: Neural is not retried while it is known to be too expensive (30 s), however long the
    // headroom lasts; then it is tried once more, fails again, and the next retry waits twice as long as before.
    CHECK(s.Until(0.2, 24 * 29) == 24 * 29 + 1 && s.governor.Tier() == QualityTier::EdgeDirected);
    const auto retried = s.Until(0.2, 24 * 3);
    CHECK(retried <= 24 * 3 && s.governor.Tier() == QualityTier::Neural);
    CHECK(s.Until(0.9, 100) == 20 && s.governor.Stats().steps_down == 2 && s.governor.Stats().steps_up == 2);
    // Hysteresis band (55-85 %): nothing moves, in either direction.
    const auto before = s.changes;
    CHECK(s.Until(0.7, 24 * 60) == 24 * 60 + 1 && s.changes == before && s.governor.Tier() == QualityTier::EdgeDirected);
}
void HardOverload() {
    Stream s; s.governor.Configure();
    // Four consecutive pictures over the whole budget: down at once, before the 20-picture p95 rule could.
    CHECK(s.Until(1.2, 10) == 4 && s.governor.Tier() == QualityTier::Fast);
    // Nothing is cheaper than Fast: overload there changes nothing.
    CHECK(s.Until(2.0, 100) == 101 && s.governor.Tier() == QualityTier::Fast);
    // A single slow picture now and then (one in 40: at most one in any window) does not move anything: the run is
    // broken at once and the p95 (the window's second largest value) stays low.
    Stream t; t.governor.Configure();
    for (unsigned i = 0; i < 400; ++i) (void)t.Picture(i % 40 == 0 ? 1.5 : 0.3);
    CHECK(t.changes == 0 && t.governor.Tier() == QualityTier::EdgeDirected);
    // One in ten is a real problem (10 % of pictures over budget): the p95 sees it and steps down.
    Stream u; u.governor.Configure();
    for (unsigned i = 0; i < 400 && !u.changes; ++i) (void)u.Picture(i % 10 == 0 ? 1.5 : 0.3);
    CHECK(u.changes == 1 && u.governor.Tier() == QualityTier::Fast);
}
void ThinQueue() {
    // A start: nothing converted ahead yet, the first picture at 55 % of the interval: down at once.
    Stream s; s.governor.Configure(); s.queued = 0;
    CHECK(s.Until(0.55, 10) == 1 && s.governor.Tier() == QualityTier::Fast && s.governor.Stats().thin == 1);
    // The queue fills (100 ms and more ahead: 3 pictures at 24 fps) at Fast's 25 %; Edge-directed is tried again once its
    // back-off (2 s) and the settle time have passed, and holds at 55 % with the queue full.
    s.queued = 3 * kBudget;
    const unsigned up = s.Until(0.25, 24 * 5);
    CHECK(up >= 2 * 24 && up <= 24 * 5 && s.governor.Tier() == QualityTier::EdgeDirected);
    CHECK(s.Until(0.55, 24 * 10) == 24 * 10 + 1 && s.governor.Stats().thin == 1);
    // A thin queue but a cheap picture (45 %): nothing.
    Stream t; t.governor.Configure(); t.queued = kBudget;
    CHECK(t.Until(0.45, 48) == 49 && t.governor.Tier() == QualityTier::EdgeDirected);
    // The queue unknown: the rules before it (55 % is no overload).
    Stream u; u.governor.Configure();
    CHECK(u.Until(0.55, 48) == 49 && u.governor.Tier() == QualityTier::EdgeDirected && u.governor.Stats().thin == 0);
}
void Noise() {
    // Alternating 50 % / 90 %: the window's p95 is 90 % (> 85 %): down after 20 pictures; up again is blocked by the
    // memory of Edge-directed's cost (30 s), so no oscillation for as long as the noise lasts.
    Stream s; s.governor.Configure();
    unsigned i = 0;
    for (; i < 200 && s.governor.Tier() == QualityTier::EdgeDirected; ++i) (void)s.Picture(i % 2 ? 0.9 : 0.5);
    CHECK(i == 20 && s.governor.Tier() == QualityTier::Fast);
    CHECK(s.Until(0.1, 24 * 25) == 24 * 25 + 1 && s.changes == 1);
}
void Backoff() {
    // Without the memory rule, the back-off alone spaces the retries of a failing tier: 2, 4, 8, 16 s.
    GovernorOptions options; options.memory = 0;
    Stream s; s.governor.Configure(options); s.governor.SetNeuralAvailable(true, s.now);
    CHECK(s.Until(0.2, 100) == 32 && s.governor.Tier() == QualityTier::Neural);
    std::int64_t failed = 0, gaps[4]{};
    for (unsigned k = 0; k < 4; ++k) {
        CHECK(s.Until(0.9, 100) == 20 && s.governor.Tier() == QualityTier::EdgeDirected); failed = s.now;
        CHECK(s.Until(0.2, 24 * 40) <= 24 * 40 && s.governor.Tier() == QualityTier::Neural); gaps[k] = s.now - failed;
    }
    std::printf("Back-off: Neural retried %.2f, %.2f, %.2f, %.2f s after failing\n", gaps[0] / 1e7, gaps[1] / 1e7, gaps[2] / 1e7, gaps[3] / 1e7);
    for (unsigned k = 0; k < 4; ++k) {
        const std::int64_t expected = 20000000ll << k;
        CHECK(gaps[k] >= expected && gaps[k] < expected + 2 * kBudget);
    }
    // Ten seconds at a tier prove it: its next failure starts the back-off over at 2 s.
    CHECK(s.Until(0.2, 24 * 11) == 24 * 11 + 1 && s.governor.Tier() == QualityTier::Neural);
    CHECK(s.Until(0.9, 100) == 2); // A full window of light pictures: two heavy ones already make the p95.
    failed = s.now;
    CHECK(s.Until(0.2, 24 * 40) <= 24 * 40 && s.now - failed < 20000000 + 2 * kBudget);
}
void ManualAndAvailability() {
    Stream s; s.governor.Configure();
    // Without neural (not upscaling), Auto never goes to Neural, whatever the headroom.
    CHECK(s.Until(0.05, 24 * 20) == 24 * 20 + 1 && s.governor.Tier() == QualityTier::EdgeDirected);
    // Manual Neural: fixed whatever the load; Auto from there adapts again (20 pictures at 90 %).
    s.governor.SetMode(QualityMode::Neural, s.now);
    CHECK(s.governor.Tier() == QualityTier::Neural && s.governor.LastChange().reason == QualityGovernor::Reason::Manual);
    CHECK(s.Until(3.0, 200) == 201 && s.governor.Tier() == QualityTier::Neural);
    s.governor.SetNeuralAvailable(true, s.now);
    s.governor.SetMode(QualityMode::Auto, s.now);
    CHECK(s.governor.Mode() == QualityMode::Auto && s.governor.Tier() == QualityTier::Neural);
    CHECK(s.Until(0.9, 100) == 20 && s.governor.Tier() == QualityTier::EdgeDirected);
    s.governor.SetMode(QualityMode::Fast, s.now); CHECK(s.governor.Tier() == QualityTier::Fast);
    CHECK(s.Until(0.01, 24 * 10) == 24 * 10 + 1);
    s.governor.SetMode(QualityMode::Auto, s.now); CHECK(s.governor.Tier() == QualityTier::Fast);
    CHECK(s.Until(0.1, 100) == 32 && s.governor.Tier() == QualityTier::EdgeDirected);
    // Neural that can no longer run (the window shrank to the video's size) in Auto: Edge-directed, no cue.
    s.governor.SetMode(QualityMode::Neural, s.now); s.governor.SetMode(QualityMode::Auto, s.now);
    CHECK(s.governor.Tier() == QualityTier::Neural);
    s.governor.SetNeuralAvailable(false, s.now);
    CHECK(s.governor.Tier() == QualityTier::EdgeDirected && s.governor.LastChange().reason == QualityGovernor::Reason::Unavailable);
    // Auto entered while Neural cannot run: Edge-directed at once.
    s.governor.SetMode(QualityMode::Neural, s.now); s.governor.SetMode(QualityMode::Auto, s.now);
    CHECK(s.governor.Tier() == QualityTier::EdgeDirected);
}
void Workload() {
    // What a tier cost is forgotten when the workload or the frame rate changes: Neural, measured too expensive at one
    // size, is tried again after a resize as soon as the usual headroom rule allows.
    Stream s; s.governor.Configure(); s.governor.SetNeuralAvailable(true, s.now);
    CHECK(s.Until(0.2, 100) == 32 && s.Until(0.9, 100) == 20 && s.governor.Tier() == QualityTier::EdgeDirected);
    CHECK(s.Until(0.2, 24 * 5) == 24 * 5 + 1); // Remembered: not retried.
    bool changed = false; unsigned n = 0;
    for (; n < 200 && !changed; ++n) changed = s.Picture(0.2, kWorkload + 1);
    CHECK(changed && n == 32 && s.governor.Tier() == QualityTier::Neural && s.governor.Known(QualityTier::EdgeDirected) != 0);
    // A different frame interval (beyond 1 %) also starts over; a rounding difference does not.
    s.now += kBudget; (void)s.governor.Observe(QualityTier::Neural, 1000, kWorkload + 1, kBudget + 1, s.now);
    CHECK(s.governor.Window().Count() == 1 && s.governor.Known(QualityTier::EdgeDirected) != 0 && s.governor.Budget() == kBudget);
    s.now += kBudget; (void)s.governor.Observe(QualityTier::Neural, 1000, kWorkload + 1, kBudget / 2, s.now);
    CHECK(s.governor.Window().Count() == 1 && s.governor.Budget() == kBudget / 2 && s.governor.Known(QualityTier::EdgeDirected) == 0);
}
}
int main() {
    std::puts("Quality governor tests"); // The first output allocates stdout's buffer (seen by the probe in MSVC Debug): before any probe.
    allocation_probe::Begin();
    Percentile(); StepUpAndDown(); HardOverload(); ThinQueue(); Noise(); Backoff(); ManualAndAvailability(); Workload();
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0);
    std::printf("Quality governor: %s (%d failures, %llu allocations)\n", failures ? "FAIL" : "PASS", failures, static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
