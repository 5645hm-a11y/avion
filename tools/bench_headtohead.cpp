// AVION's side of the head-to-head benchmark (docs/benchmarks.md; tools/bench/run_headtohead.py runs both sides). Portable:
// MSVC on Windows, GCC on Linux, where a malloc-counting LD_PRELOAD library (tools/bench/malloc_count.c) counts every heap
// allocation in the process through `bench_alloc_count`, exactly as it does for ffmpeg and OpenCV.
//   bench_headtohead scan <file.mp4> <pictures> [frames] the bitstream inspector over the file's video samples (frames: each picture on stderr)
//   bench_headtohead flow <gray.raw> <w> <h> <pictures> video::Perception (tile flow by predicted diamond search, TTC, objects)
//   bench_headtohead survey <gray.raw> <w> <h> <pictures> video::HorizonLevel's structure-tensor survey at each SIMD level
// Each prints one JSON line. Everything comes from one arena made before the measurements; nothing is allocated inside them.
#include "media/codec/BitstreamPerception.hpp"
#include "media/core/linear_memory_arena.hpp"
#include "media/core/spsc_ring_buffer.hpp"
#include "media/demux/IsobmffParser.hpp"
#include "media/platform/FileBlockSource.hpp"
#include "media/video/HorizonLevel.hpp"
#include "media/video/Perception.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using namespace media;
using Clock = std::chrono::steady_clock;
// A setup failure, with where it happened.
#define FAIL() (std::fprintf(stderr, "setup failed at line %d\n", __LINE__), 2)
double Micro(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

// Heap allocations so far in this process (Linux under tools/bench/malloc_count.so); -1 when nothing counts them.
long long Allocations() noexcept {
#if defined(_WIN32)
    return -1;
#else
    using Count = unsigned long long (*)();
    static const auto count = reinterpret_cast<Count>(dlsym(RTLD_DEFAULT, "bench_alloc_count"));
    return count ? static_cast<long long>(count()) : -1;
#endif
}
// Allocations since `before` (-1: not counted on this platform).
long long Since(long long before) noexcept { const long long now = Allocations(); return before < 0 || now < 0 ? -1 : now - before; }
const char* Platform() {
#if defined(_WIN32)
    return "windows-msvc";
#else
    return "linux-gcc";
#endif
}
// Percentiles of `n` per-call times (sorted in place).
double Percentile(double* v, unsigned n, double p) { std::sort(v, v + n); return v[std::min(n - 1, static_cast<unsigned>(p * (n - 1) + 0.5))]; }

// The portable file source's blocks are 1 MiB; the parser refuses a track whose largest sample a block cannot hold (a 4K
// HEVC intra picture can be larger). Here the samples are read with ReadAt, so only the tables go through the blocks.
class WholeSamples final : public platform::IBlockSource {
public:
    explicit WholeSamples(platform::FileBlockSource& file) noexcept : file_(file) {}
    [[nodiscard]] std::uint64_t Size() const noexcept override { return file_.Size(); }
    [[nodiscard]] std::uint32_t MaxRangeBytes() const noexcept override { return 64u << 20; }
    [[nodiscard]] platform::IoStatus Request(std::uint64_t offset, std::uint32_t bytes, platform::BlockToken& token) noexcept override { return file_.Request(offset, bytes, token); }
    [[nodiscard]] platform::IoStatus Poll(platform::BlockToken token, platform::BlockView& view) noexcept override { return file_.Poll(token, view); }
    [[nodiscard]] bool Release(platform::BlockToken token) noexcept override { return file_.Release(token); }
private:
    platform::FileBlockSource& file_;
};

int Scan(const char* path, unsigned pictures, bool dump) {
    core::LinearMemoryArena arena;
    if (!arena.Initialize(768ull << 20)) return FAIL();
    platform::FileBlockSource source;
    if (!source.Open(arena, path)) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
    WholeSamples tables(source);
    demux::IsobmffParser parser;
    auto* queue = arena.Create<core::SpscRingBuffer<core::MediaPacket, 8>>();
    if (!queue || !parser.Initialize(arena, tables, demux::ParserLimits{4, demux::ParserLimits{}.index_bytes, 1})) return FAIL();
    // The sample tables are complete once the parser emits (Seekable); the packets it queues meanwhile are not used.
    for (unsigned guard = 0; !parser.Seekable() && guard < 1000000; ++guard) {
        const auto status = parser.Step(*queue);
        if (status == demux::ParseStatus::Failed || status == demux::ParseStatus::Done) break;
    }
    if (!parser.Seekable()) { std::fprintf(stderr, "no sample tables\n"); return 2; }
    unsigned track = UINT32_MAX;
    for (unsigned i = 0; i < parser.TrackCount(); ++i) {
        const auto* t = parser.Track(i);
        std::fprintf(stderr, "track %u: codec %u, %u samples, refusal %u\n", i, static_cast<unsigned>(t->codec), t->sample_count, static_cast<unsigned>(t->refusal));
        if (track == UINT32_MAX && t->refusal == demux::ParseError::None && (t->codec == demux::Codec::H264 || t->codec == demux::Codec::Hevc)) track = i;
    }
    if (track == UINT32_MAX) { std::fprintf(stderr, "no H.264 or HEVC track\n"); return 2; }
    const auto& info = *parser.Track(track);
    pictures = std::min(pictures, info.sample_count);
    auto* config = static_cast<std::uint8_t*>(arena.Allocate(static_cast<std::size_t>(info.configuration.bytes), 8));
    auto* offsets = static_cast<std::uint64_t*>(arena.Allocate(sizeof(std::uint64_t) * (pictures + 1), 8));
    auto* calls = static_cast<double*>(arena.Allocate(sizeof(double) * pictures, 8));
    auto* verdict = static_cast<float*>(arena.Allocate(sizeof(float) * pictures, 8)); // Each picture's final cut probability.
    if (!config || !offsets || !calls || !verdict || !source.ReadAt(info.configuration.offset, config, static_cast<std::uint32_t>(info.configuration.bytes))) return FAIL();
    std::uint64_t total = 0;
    std::int64_t interval = 0; // The first picture's duration: another tool's cut times become picture numbers.
    for (unsigned s = 0; s < pictures; ++s) {
        demux::IsobmffParser::SampleRecord r{};
        if (!parser.SampleAt(track, s, r)) return FAIL();
        total += r.bytes; if (!s) interval = r.duration_100ns;
    }
    auto* units = static_cast<std::uint8_t*>(arena.Allocate(static_cast<std::size_t>(total), 64));
    if (!units) { std::fprintf(stderr, "arena too small for %llu bytes\n", static_cast<unsigned long long>(total)); return 2; }
    const std::uint32_t codec = info.codec == demux::Codec::H264 ? codec::BitstreamPerception::kH264 : codec::BitstreamPerception::kHevc;
    auto* inspector = arena.Create<codec::BitstreamPerception>();
    if (!inspector) return FAIL();
    (void)arena.Seal();

    // Pass 1, end to end: each sample read from the file by its table entry, then inspected (what a host's scan costs).
    const long long before_read = Allocations();
    if (!inspector->Configure(codec, config, static_cast<std::uint32_t>(info.configuration.bytes), info.nal_length_bytes)) return FAIL();
    codec::BitstreamFrame frame{};
    unsigned cuts = 0, confirmed = 0, qp_known = 0;
    auto start = Clock::now();
    offsets[0] = 0;
    for (unsigned s = 0; s < pictures; ++s) {
        demux::IsobmffParser::SampleRecord r{};
        (void)parser.SampleAt(track, s, r);
        if (!source.ReadAt(r.offset, units + offsets[s], r.bytes)) return FAIL();
        offsets[s + 1] = offsets[s] + r.bytes;
        (void)inspector->Inspect(units + offsets[s], r.bytes, frame);
    }
    const double end_to_end = Micro(Clock::now() - start);
    const long long read_allocations = Since(before_read);

    // Pass 2, the inspector alone over the same units in memory, timed as a whole (the clock's own cost stays out).
    *inspector = codec::BitstreamPerception{};
    (void)inspector->Configure(codec, config, static_cast<std::uint32_t>(info.configuration.bytes), info.nal_length_bytes);
    const long long before = Allocations();
    start = Clock::now();
    for (unsigned s = 0; s < pictures; ++s) {
        (void)inspector->Inspect(units + offsets[s], static_cast<std::uint32_t>(offsets[s + 1] - offsets[s]), frame);
        cuts += frame.scene_cut >= 0.5f ? 1u : 0u;              // Candidates when inspected...
        verdict[s] = frame.scene_cut;                           // ...each picture's final word: its first look, or its revision.
        if (frame.revised && frame.revised_index < pictures) verdict[frame.revised_index] = frame.revised_scene_cut;
        confirmed += frame.revised && frame.revised_scene_cut >= 0.95f ? 1u : 0u; // ...and those the next picture confirmed.
        qp_known += frame.qp >= 0 ? 1u : 0u;
    }
    const double inspect = Micro(Clock::now() - start);
    const long long allocations = Since(before);

    // Pass 3, per call, for the distribution.
    *inspector = codec::BitstreamPerception{};
    (void)inspector->Configure(codec, config, static_cast<std::uint32_t>(info.configuration.bytes), info.nal_length_bytes);
    for (unsigned s = 0; s < pictures; ++s) {
        const auto t = Clock::now();
        (void)inspector->Inspect(units + offsets[s], static_cast<std::uint32_t>(offsets[s + 1] - offsets[s]), frame);
        calls[s] = Micro(Clock::now() - t);
    }
    // Pass 4 (`scan <file> <pictures> frames`), untimed: one line per picture on stderr, for reading what the inspector saw.
    if (dump) {
        *inspector = codec::BitstreamPerception{};
        (void)inspector->Configure(codec, config, static_cast<std::uint32_t>(info.configuration.bytes), info.nal_length_bytes);
        for (unsigned s = 0; s < pictures; ++s) {
            (void)inspector->Inspect(units + offsets[s], static_cast<std::uint32_t>(offsets[s + 1] - offsets[s]), frame);
            std::fprintf(stderr, "%u %c%s L%u d%u qp=%d bytes=%u cx=%.4f act=%.3f burst=%.2f cut=%.2f", s, frame.type, frame.key ? "k" : "", frame.layer,
                         frame.reference_distance, frame.qp, frame.bytes, frame.complexity, frame.activity, frame.burst, frame.scene_cut);
            if (frame.revised) std::fprintf(stderr, " revised %u %.2f", frame.revised_index, frame.revised_scene_cut);
            std::fputc('\n', stderr);
        }
    }
    std::printf("{\"bench\":\"avion_scan\",\"platform\":\"%s\",\"codec\":\"%s\",\"width\":%u,\"height\":%u,\"pictures\":%u,\"bytes\":%llu,"
                "\"inspect_us_mean\":%.4f,\"inspect_us_p50\":%.4f,\"inspect_us_p99\":%.4f,\"end_to_end_us_mean\":%.4f,"
                "\"allocations_inspect\":%lld,\"allocations_read_inspect\":%lld,\"cuts\":%u,\"confirmed_cuts\":%u,\"qp_known\":%u,\"frame_100ns\":%lld,\"final_cuts\":[",
                Platform(), codec == codec::BitstreamPerception::kH264 ? "h264" : "hevc", info.width, info.height, pictures,
                static_cast<unsigned long long>(total), inspect / pictures, Percentile(calls, pictures, 0.5), Percentile(calls, pictures, 0.99),
                end_to_end / pictures, allocations, read_allocations, cuts, confirmed, qp_known, static_cast<long long>(interval));
    // The pictures (decode order) finally judged cuts (0.5 or more), for matching against another detector's.
    for (unsigned s = 0, n = 0; s < pictures; ++s) if (verdict[s] >= 0.5f) std::printf(n++ ? ",%u" : "%u", s);
    std::puts("]}");
    return 0;
}

// The raw 8-bit planes, read whole into the arena before anything is measured.
const std::uint8_t* LoadRaw(core::LinearMemoryArena& arena, const char* path, std::size_t bytes) {
    auto* data = static_cast<std::uint8_t*>(arena.Allocate(bytes, 64));
    std::FILE* f = data ? platform::OpenUtf8(path) : nullptr;
    if (!f) return nullptr;
    const bool whole = std::fread(data, 1, bytes, f) == bytes;
    std::fclose(f);
    return whole ? data : nullptr;
}

int Flow(const char* path, unsigned w, unsigned h, unsigned pictures) {
    core::LinearMemoryArena arena;
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    if (!arena.Initialize(plane * pictures + video::Perception::ArenaBytes(w) + (16u << 20))) return FAIL();
    const auto* planes = LoadRaw(arena, path, plane * pictures);
    auto* calls = static_cast<double*>(arena.Allocate(sizeof(double) * pictures, 8));
    auto* perception = arena.Create<video::Perception>();
    if (!planes || !calls || !perception || !perception->Configure(arena, w, h, 1.2f)) { std::fprintf(stderr, "flow setup failed\n"); return 2; }
    (void)arena.Seal();
    constexpr unsigned kWarm = 30; // The first pictures fill the history; measured from then on.
    std::int64_t pts = 0;
    for (unsigned f = 0; f < kWarm; ++f, pts += 166833) (void)perception->Push(planes + plane * f, w, pts, {});
    const long long before = Allocations();
    unsigned looming = 0, objects = 0;
    const auto start = Clock::now();
    for (unsigned f = kWarm; f < pictures; ++f, pts += 166833) {
        const auto t = Clock::now();
        const auto& r = perception->Push(planes + plane * f, w, pts, {});
        calls[f - kWarm] = Micro(Clock::now() - t);
        looming += r.looming ? 1u : 0u; objects += r.object_count;
    }
    const unsigned n = pictures - kWarm;
    const double total = Micro(Clock::now() - start);
    const long long allocations = Since(before);
    std::printf("{\"bench\":\"avion_flow\",\"platform\":\"%s\",\"simd_level\":%u,\"width\":%u,\"height\":%u,\"pictures\":%u,\"ms_mean\":%.4f,"
                "\"ms_p50\":%.4f,\"ms_p99\":%.4f,\"allocations\":%lld,\"looming\":%u,\"objects\":%u}\n",
                Platform(), video::Perception::Level(), w, h, n, total / n / 1000, Percentile(calls, n, 0.5) / 1000, Percentile(calls, n, 0.99) / 1000,
                allocations, looming, objects);
    return 0;
}

int Survey(const char* path, unsigned w, unsigned h, unsigned pictures) {
    core::LinearMemoryArena arena;
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    if (!arena.Initialize(plane * pictures + (1u << 20))) return FAIL();
    const auto* planes = LoadRaw(arena, path, plane * pictures);
    if (!planes) { std::fprintf(stderr, "survey setup failed\n"); return 2; }
    (void)arena.Seal();
    static std::uint32_t words[video::HorizonLevel::kWords];
    std::printf("{\"bench\":\"avion_survey\",\"platform\":\"%s\",\"width\":%u,\"height\":%u,\"pictures\":%u,\"levels\":[", Platform(), w, h, pictures);
    const unsigned top = video::HorizonLevel::ForceLevel(2);
    for (unsigned level = 0; level <= top; ++level) {
        video::HorizonLevel::ForceLevel(level);
        // Best of 5 passes over every picture: the time a picture costs when nothing else competes for the core.
        double best = 1e30;
        long long allocations = 0;
        for (unsigned pass = 0; pass < 5; ++pass) {
            const long long before = Allocations();
            const auto start = Clock::now();
            for (unsigned f = 0; f < pictures; ++f) {
                std::memset(words, 0, sizeof(words));
                video::HorizonLevel::Accumulate(planes + plane * f, w, w, h, words);
            }
            best = std::min(best, Micro(Clock::now() - start) / pictures);
            const long long counted = Since(before);
            allocations = counted < 0 || allocations < 0 ? -1 : allocations + counted;
        }
        std::printf("%s{\"level\":\"%s\",\"ms_mean\":%.4f,\"allocations\":%lld}", level ? "," : "", level == 0 ? "scalar" : level == 1 ? "sse2" : "avx2",
                    best / 1000, allocations);
    }
    std::printf("]}\n");
    return 0;
}
}

int main(int argc, char** argv) {
    const auto number = [&](int i) { return static_cast<unsigned>(std::strtoul(argv[i], nullptr, 10)); };
    if (argc >= 4 && std::strcmp(argv[1], "scan") == 0) return Scan(argv[2], number(3), argc >= 5 && std::strcmp(argv[4], "frames") == 0);
    if (argc >= 6 && std::strcmp(argv[1], "flow") == 0) return Flow(argv[2], number(3), number(4), number(5));
    if (argc >= 6 && std::strcmp(argv[1], "survey") == 0) return Survey(argv[2], number(3), number(4), number(5));
    std::fputs("usage: bench_headtohead scan <file.mp4> <pictures> | flow|survey <gray.raw> <width> <height> <pictures>\n", stderr);
    return 1;
}
