// The flight recorder and the telemetry readout (docs/session.md, "Flight recorder", "Telemetry readout"):
// - the recorder keeps exactly its newest Capacity records, in order, numbered; a reader racing a writer never gets a
//   record mixed from two (each one read is checked against its own number), and readers never block the writer;
// - two recorders merge into time order, newest kept;
// - the readout states the engine's numbers as given (the explanation of a tier change names the tier left, the p95 it
//   measured, the threshold and the frame), never more lines than the panel has, never a character cut in two;
// - none of it allocates.
#include "media/core/flight_recorder.hpp"
#include "media/ui/TelemetryReadout.hpp"
#include "allocation_probe.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>
using media::core::FlightEvent; using media::core::FlightRecord; using media::core::FlightRecorder;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr std::int64_t S = 10000000, Ms = 10000;
// A record that carries its own number in every field: a mixture of two records cannot pass Whole().
FlightRecord Numbered(std::uint64_t n) {
    return {static_cast<std::int64_t>(n), static_cast<FlightEvent>(1 + n % (static_cast<unsigned>(FlightEvent::Count) - 1)), static_cast<std::uint16_t>(n & 0xffff),
            static_cast<std::uint32_t>(n >> 16), static_cast<std::int64_t>(n * 3 + 1), ~static_cast<std::int64_t>(n)};
}
bool Whole(const FlightRecord& r) {
    const auto n = static_cast<std::uint64_t>(r.time);
    const auto e = Numbered(n);
    return r.kind == e.kind && r.a == e.a && r.b == e.b && r.c == e.c && r.d == e.d;
}
FlightRecorder<256> recorder; // Fixed storage (10 KiB); static so that the stack stays small.
FlightRecord out[256], other[256], scratch_a[256], scratch_b[256];
void Bounds() {
    CHECK(recorder.Written() == 0 && recorder.Snapshot(out, 256) == 0);
    for (std::uint64_t n = 0; n < 100; ++n) recorder.Record(Numbered(n));
    std::uint64_t first = 99;
    CHECK(recorder.Snapshot(out, 256, &first) == 100 && first == 0 && out[0].time == 0 && out[99].time == 99);
    for (std::uint64_t n = 100; n < 1000; ++n) recorder.Record(Numbered(n));
    // The newest 256, oldest first, numbered from 744; asking for fewer gives the newest that many.
    CHECK(recorder.Written() == 1000 && recorder.Snapshot(out, 1000, &first) == 256 && first == 744);
    bool ordered = true; for (unsigned i = 0; i < 256; ++i) ordered = ordered && out[i].time == static_cast<std::int64_t>(744 + i) && Whole(out[i]);
    CHECK(ordered);
    CHECK(recorder.Snapshot(out, 10, &first) == 10 && first == 990 && out[9].time == 999 && out[0].time == 990);
    CHECK(recorder.Snapshot(nullptr, 10) == 0 && recorder.Snapshot(out, 0) == 0);
    // The newest of a kind, among those still held.
    FlightRecord last{};
    const auto kind = Numbered(997).kind;
    CHECK(recorder.Last(kind, last) && last.time >= 997 - static_cast<std::int64_t>(FlightEvent::Count) && Whole(last) && last.kind == kind);
    FlightRecorder<4> tiny; tiny.Record(Numbered(1));
    CHECK(!tiny.Last(FlightEvent::None, last) && !tiny.Last(Numbered(2).kind, last) && tiny.Last(Numbered(1).kind, last) && last.time == 1);
    // Two recorders merged: time order, the newest of both kept.
    FlightRecorder<8> ui, worker;
    for (std::int64_t t : {10, 30, 50, 70, 90}) ui.Record(t, FlightEvent::SeekRequested, 0, static_cast<std::uint32_t>(t));
    for (std::int64_t t : {20, 40, 60, 80}) worker.Record(t, FlightEvent::SeekApplied, 0, static_cast<std::uint32_t>(t));
    const unsigned merged = media::core::MergeNewest(ui, worker, other, 6, scratch_a, scratch_b);
    CHECK(merged == 6 && other[0].time == 40 && other[5].time == 90);
    bool sorted = true; for (unsigned i = 1; i < merged; ++i) sorted = sorted && other[i - 1].time < other[i].time && other[i].b == static_cast<std::uint32_t>(other[i].time);
    CHECK(sorted && other[0].kind == FlightEvent::SeekApplied && other[5].kind == FlightEvent::SeekRequested);
    CHECK(media::core::MergeNewest(ui, worker, other, 32, scratch_a, scratch_b) == 9 && other[0].time == 10);
}
// A writer filling the ring as fast as it can while a reader takes snapshots: every record read is whole and in order.
void Race() {
    static FlightRecorder<256> ring;
    constexpr std::uint64_t kRecords = 3000000;
    std::atomic<bool> done{false};
    std::uint64_t snapshots = 0, records = 0, broken = 0, disorder = 0, gaps = 0;
    std::thread writer([&] { for (std::uint64_t n = 0; n < kRecords; ++n) ring.Record(Numbered(n)); done.store(true); });
    while (!done.load()) {
        std::uint64_t first = 0;
        const unsigned n = ring.Snapshot(out, 256, &first);
        ++snapshots; records += n;
        for (unsigned i = 0; i < n; ++i) {
            if (!Whole(out[i])) ++broken;
            if (i && out[i].time <= out[i - 1].time) ++disorder;
            if (i && out[i].time != out[i - 1].time + 1) ++gaps; // Records overwritten while read are left out, never mixed.
        }
    }
    writer.join();
    std::uint64_t first = 0;
    const unsigned final_count = ring.Snapshot(out, 256, &first);
    std::printf("Race: %llu records written, %llu snapshots read %llu records: %llu torn, %llu out of order, %llu left out mid-snapshot\n",
                static_cast<unsigned long long>(kRecords), static_cast<unsigned long long>(snapshots), static_cast<unsigned long long>(records),
                static_cast<unsigned long long>(broken), static_cast<unsigned long long>(disorder), static_cast<unsigned long long>(gaps));
    CHECK(broken == 0 && disorder == 0 && snapshots > 0 && final_count == 256 && first == kRecords - 256 && ring.Written() == kRecords);
}
bool WellFormed(std::string_view text) {
    while (!text.empty()) {
        const auto lead = static_cast<unsigned char>(text.front());
        const std::size_t n = lead < 0x80 ? 1 : lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 0;
        if (!n || n > text.size()) return false;
        for (std::size_t i = 1; i < n; ++i) if ((static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) return false;
        text.remove_prefix(n);
    }
    return true;
}
unsigned Lines(std::string_view text) { unsigned n = text.empty() ? 0 : 1; for (const char c : text) n += c == '\n' ? 1u : 0u; return n; }
bool Has(std::string_view text, std::string_view part) { return text.find(part) != text.npos; }
void Readout() {
    using media::render::QualityTier; using media::render::QualityMode; using Reason = media::render::QualityGovernor::Reason;
    char text[384];
    media::ui::TelemetrySnapshot s;
    s.now = 100 * S; s.mode = QualityMode::Auto; s.tier = QualityTier::EdgeDirected; s.timing = true; s.p95 = 61 * Ms / 10; s.budget = 166667; s.samples = 32;
    s.picture = true; s.codec = "HEVC"; s.bits = 10; s.transfer = "PQ"; s.source_width = 3840; s.source_height = 2160; s.shown_width = 1920; s.shown_height = 1080;
    s.audio = true; s.sample_rate = 48000; s.source_channels = 6; s.output_channels = 2; s.fold_down = true; s.binaural = true;
    s.presented = 1204; s.dropped = 0; s.tiles_reused = 35; s.tiles_evaluated = 65;
    // No change yet: the tier, what it costs, nothing invented.
    auto n = media::ui::ExplainQuality(s, text, sizeof(text));
    std::printf("Unchanged: %s\n", text);
    CHECK(std::string_view(text, n) == "Quality \xc2\xb7 Edge-directed has not changed\nGPU 6.1 ms per picture (p95) of a 16.7 ms frame");
    // A step down for overload: the tier left, its measured p95, the threshold (85 % of the frame) and the frame.
    s.changes = 1; s.last_change = {88 * S, QualityTier::Neural, QualityTier::EdgeDirected, Reason::Overload, 182 * Ms / 10, 166667};
    n = media::ui::ExplainQuality(s, text, sizeof(text));
    std::printf("Overload: %s\n", text);
    CHECK(std::string_view(text, n) == "Quality \xc2\xb7 Edge-directed since 12 s ago\nNeural super-resolution needed 18.2 ms per picture, over the whole 16.7 ms frame\nNow 6.1 ms per picture (p95)");
    s.last_change.p95 = 150 * Ms / 10; // Over 85 % (14.2 ms), under the frame.
    n = media::ui::ExplainQuality(s, text, sizeof(text));
    CHECK(Has({text, n}, "needed 15.0 ms per picture, over 85% of the 16.7 ms frame (14.2 ms)"));
    // A step up: the tier left was cheap enough.
    s.tier = QualityTier::Neural; s.last_change = {100 * S - 5000000, QualityTier::EdgeDirected, QualityTier::Neural, Reason::Headroom, 41 * Ms / 10, 166667};
    n = media::ui::ExplainQuality(s, text, sizeof(text));
    std::printf("Headroom: %s\n", text);
    CHECK(Has({text, n}, "Quality \xc2\xb7 Neural super-resolution since just now") && Has({text, n}, "Edge-directed took 4.1 ms, under 55% of the 16.7 ms frame: room for the better tier"));
    // By hand, and a tier that cannot run.
    s.mode = QualityMode::Fast; s.tier = QualityTier::Fast; s.last_change = {40 * S, QualityTier::Neural, QualityTier::Fast, Reason::Manual, 0, 166667};
    n = media::ui::ExplainQuality(s, text, sizeof(text));
    CHECK(Has({text, n}, "since 60 s ago") && Has({text, n}, "Chosen by hand") && !Has({text, n}, "Now"));
    s.mode = QualityMode::Auto; s.tier = QualityTier::EdgeDirected; s.last_change = {95 * S, QualityTier::Neural, QualityTier::EdgeDirected, Reason::Unavailable, 0, 166667};
    n = media::ui::ExplainQuality(s, text, sizeof(text));
    CHECK(Has({text, n}, "cannot run: the picture is not being enlarged"));
    // No samples at the new tier yet: no "Now" line (a 0.0 ms would be a number never measured).
    s.samples = 0; n = media::ui::ExplainQuality(s, text, sizeof(text)); CHECK(!Has({text, n}, "Now")); s.samples = 32;
    // The panel: every section, the engine's own numbers, at most kReadoutLines lines.
    s.last_change = {88 * S, QualityTier::Neural, QualityTier::EdgeDirected, Reason::Overload, 182 * Ms / 10, 166667};
    s.loop = true; s.loop_a = 12 * S + 4 * S / 10; s.loop_b = 15 * S + 8 * S / 10; s.loop_wraps = 3;
    const FlightRecord events[]{{90 * S, FlightEvent::SeekRequested, 0, 2, 12 * S, 0}, {95 * S, FlightEvent::SeekLanded, 0, 2, 12 * S + 4 * S / 10, 38 * Ms}, {96 * S, FlightEvent::Stats, 0, 0, 0, 0}};
    s.events = events; s.event_count = 3;
    n = media::ui::ComposeStats(s, text, sizeof(text));
    std::printf("Panel:\n%s\n", text);
    const std::string_view panel{text, n};
    CHECK(Has(panel, "Quality \xc2\xb7 Edge-directed (auto) \xc2\xb7 GPU 6.1 ms p95 of 16.7 ms\n"));
    CHECK(Has(panel, "Why \xc2\xb7 Neural needed 18.2 ms, over the whole 16.7 ms frame \xc2\xb7 12 s ago\n"));
    CHECK(Has(panel, "Picture \xc2\xb7 HEVC 10-bit PQ 3840\xc3\x97" "2160 \xe2\x86\x92 1920\xc3\x97" "1080\n"));
    CHECK(Has(panel, "Audio \xc2\xb7 48 kHz 5.1 \xe2\x86\x92 binaural stereo\n"));
    CHECK(Has(panel, "Frames \xc2\xb7 1204 shown \xc2\xb7 0 dropped \xc2\xb7 tile reuse 35%\n"));
    CHECK(Has(panel, "Loop \xc2\xb7 0:12.4 \xe2\x80\x93 0:15.8 \xc2\xb7 3 turns\n"));
    CHECK(Has(panel, "Last \xc2\xb7 Seek landed at 0:12.4 in 38.0 ms \xc2\xb7 5 s ago") && panel.back() != '\n');
    CHECK(Lines(panel) <= media::ui::kReadoutLines && WellFormed(panel));
    // Music mode, no audio, a drop: said as it is.
    media::ui::TelemetrySnapshot music; music.now = S; music.picture = false; music.audio = true; music.sample_rate = 44100; music.source_channels = 2; music.output_channels = 2;
    music.muted = true; music.speed = 1.5f;
    n = media::ui::ComposeStats(music, text, sizeof(text));
    CHECK(Has({text, n}, "Picture \xc2\xb7 none (music mode)") && Has({text, n}, "Audio \xc2\xb7 44.1 kHz stereo \xc2\xb7 muted \xc2\xb7 1.50\xc3\x97") && !Has({text, n}, "Frames"));
    s.dropped = 2; s.drop_waited = 1; s.audio = false; s.loop = false;
    n = media::ui::ComposeStats(s, text, sizeof(text));
    CHECK(Has({text, n}, "2 dropped (last waited for its GPU conversion)") && Has({text, n}, "Audio \xc2\xb7 none"));
    // Too little room: whole parts only, never a character cut, always terminated.
    for (unsigned capacity = 1; capacity < 200; capacity += 7) {
        n = media::ui::ComposeStats(s, text, capacity);
        CHECK(n < capacity && text[n] == 0 && WellFormed({text, n}));
    }
    // Every event kind has words.
    bool described = true;
    for (unsigned k = 1; k < static_cast<unsigned>(FlightEvent::Count); ++k) {
        const FlightRecord r{S, static_cast<FlightEvent>(k), 1, 2, 3 * S, 4 * S};
        n = media::ui::DescribeEvent(r, 5 * S, text, sizeof(text));
        described = described && n > 4 && WellFormed({text, n}) && Has({text, n}, "4 s ago");
    }
    CHECK(described);
}
}
int main() {
    std::puts("Telemetry and flight recorder tests"); // The first output allocates stdout's buffer: before any probe.
    allocation_probe::Begin();
    Bounds();
    const auto recorded = allocation_probe::End();
    Race(); // Starting a thread allocates: outside the probe.
    std::puts("Readouts:");
    allocation_probe::Begin();
    Readout();
    const auto readouts = allocation_probe::End();
    std::printf("Allocations: %llu in the recorders, %llu in the readouts\n", static_cast<unsigned long long>(recorded), static_cast<unsigned long long>(readouts));
    CHECK(recorded == 0);
    std::printf("Telemetry: %d failures\n", failures);
    return failures ? 1 : 0;
}
