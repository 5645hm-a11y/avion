// WASAPI capture into the lock-free PCM ring (the ingest end of the offline voice path).
// - `loopback` (the automated test): the production render sink plays a quiet 1 kHz tone on the default endpoint and
//   a loopback capture of that endpoint must deliver it: mono frames at the capture rate, read by a second thread
//   while the owner pumps, the tone dominant in them, nothing dropped and nothing allocated. A second capture with a
//   tiny ring and no reader must drop the newest frames and carry on.
// - `vad` (automated): the sink plays silence, one synthetic word and silence again; a reader thread drains the
//   loopback capture straight into MicroVad, which must deliver the word as one utterance and discard the rest.
// - `microphone` (run by hand: it opens the default microphone for 0.3 s and discards what it hears).
// Both automated tests capture what this process renders (CaptureSource::ProcessLoopback, 2026-11-03): another program
// playing sound (a browser) is not heard, so they run their assertions instead of skipping. Where Windows has no process
// loopback they fall back to the endpoint's loopback and its skips.
#include "media/platform/windows/Win32WasapiCapture.hpp"
#include "media/platform/windows/Win32WasapiAudioSink.hpp"
#include "media/audio/MicroVad.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace media;
using namespace media::platform;
using namespace media::platform::windows;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr double kPi = 3.14159265358979323846;
float recorded[16000 * 8];
// Power of one frequency in a signal (Goertzel), per sample.
double Power(const float* samples, unsigned count, double frequency, double rate) {
    const double k = 2 * std::cos(2 * kPi * frequency / rate); double a = 0, b = 0;
    for (unsigned i = 0; i < count; ++i) { const double c = samples[i] + k * a - b; b = a; a = c; }
    return count ? (a * a + b * b - k * a * b) / (static_cast<double>(count) * count) : 0;
}
// Some endpoints (seen with "Headphones (Realtek)" as the default output) hand the loopback stream silence instead of what
// is played: every packet flagged silent (91 of 91), or all but one (90 of 91: the first, flagged a discontinuity, carried
// zeros). With the stimulus not heard either, the endpoint's audio cannot be tested: a skip (77), never a pass, and never
// a failure of the capture code. When the stimulus is heard, every check runs whatever the flags said.
bool EndpointSilent(const CaptureStatistics& s, std::uint64_t rendered, bool heard) {
    return rendered && s.packets && s.silent * 10 >= s.packets * 9 && !heard;
}
// What this test process renders (process loopback, 2026-11-03: a browser playing sound meanwhile is not heard, so neither
// test has to skip for it), or, where Windows has no process loopback, the default render endpoint's loopback.
EndpointStatus PrepareLoopback(Win32WasapiCapture& capture, core::LinearMemoryArena& arena, CaptureOptions options, bool& process) {
    options.source = CaptureSource::ProcessLoopback;
    process = capture.Prepare(arena, options) == EndpointStatus::Ready;
    if (process) return EndpointStatus::Ready;
    std::printf("process loopback unavailable (%08lx): the endpoint's loopback, which hears every program\n", static_cast<unsigned long>(capture.LastError()));
    options.source = CaptureSource::Loopback;
    return capture.Prepare(arena, options);
}
int TestMicrophone() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    Win32WasapiCapture capture;
    if (capture.Prepare(arena) != EndpointStatus::Ready) { std::printf("SKIP no usable microphone (%08lx)\n", static_cast<unsigned long>(capture.LastError())); return 77; }
    CHECK(arena.Seal());
    allocation_probe::Begin();
    CHECK(capture.Start() == EndpointStatus::Ready);
    unsigned total = 0; double energy = 0;
    const auto end = GetTickCount64() + 300;
    while (GetTickCount64() < end) {
        const auto pumped = capture.Pump(); CHECK(pumped == EndpointStatus::Ready || pumped == EndpointStatus::WouldBlock);
        const auto n = capture.Read(recorded);
        for (unsigned i = 0; i < n; ++i) energy += static_cast<double>(recorded[i]) * recorded[i];
        total += n; Sleep(1);
    }
    CHECK(capture.Stop() == EndpointStatus::Ready);
    const auto allocations = allocation_probe::End();
    std::memset(recorded, 0, sizeof(recorded)); // Nothing heard is kept.
    const auto& s = capture.Stats();
    std::printf("Microphone: endpoint %u Hz %u ch -> mono %u Hz (%s); %u frames in %u packets, %u discontinuities, %llu dropped, level %.1f dBFS, allocations %llu\n",
                capture.EndpointRate(), capture.EndpointChannels(), capture.SampleRate(), capture.Converting() ? "converted by Windows" : "endpoint format, averaged here",
                total, s.packets, s.discontinuities, static_cast<unsigned long long>(s.dropped), total && energy > 0 ? 10 * std::log10(energy / total) : -120.0,
                static_cast<unsigned long long>(allocations));
    CHECK(total >= capture.SampleRate() / 5 && s.dropped == 0 && allocations == 0 && total == s.frames);
    return failures ? 1 : 0;
}
int TestLoopback() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
    Win32WasapiAudioSink sink;
    if (sink.Prepare(arena) != EndpointStatus::Ready) { std::printf("SKIP no render endpoint (%08lx)\n", static_cast<unsigned long>(sink.LastError())); return 77; }
    Win32WasapiCapture capture, bounded;
    CaptureOptions options; options.source = CaptureSource::Loopback;
    CaptureOptions tiny = options; tiny.ring_frames = 1024;
    bool process = false, process_tiny = false;
    const auto prepared = PrepareLoopback(capture, arena, options, process);
    if (prepared != EndpointStatus::Ready) std::printf("loopback capture failed: %08lx\n", static_cast<unsigned long>(capture.LastError()));
    std::printf("Loopback source: %s\n", process ? "this process (process loopback)" : "the default endpoint");
    CHECK(prepared == EndpointStatus::Ready); CHECK(PrepareLoopback(bounded, arena, tiny, process_tiny) == EndpointStatus::Ready);
    // Misuse is refused without touching the endpoint.
    Win32WasapiCapture invalid; CaptureOptions bad = options; bad.ring_frames = 1000;
    CHECK(invalid.Prepare(arena, bad) == EndpointStatus::InvalidArgument); CHECK(invalid.Start() == EndpointStatus::InvalidArgument);
    CHECK(capture.Prepare(arena, options) == EndpointStatus::InvalidArgument);
    if (failures) return 1;
    CHECK(arena.Seal());
    const auto format = sink.Format(); // 48 kHz stereo float32.
    std::uint64_t phase = 0;
    const auto feed = [&] {
        for (;;) {
            AudioWriteSpan span{};
            if (sink.TryAcquire(4096, span) != EndpointStatus::Ready) return;
            auto* out = reinterpret_cast<float*>(span.data);
            for (std::uint32_t i = 0; i < span.frames; ++i, ++phase) {
                const auto v = static_cast<float>(0.05 * std::sin(2 * kPi * 1000.0 * static_cast<double>(phase) / format.sample_rate));
                for (unsigned c = 0; c < format.channels; ++c) out[i * format.channels + c] = v;
            }
            if (sink.Commit(span.lease, span.frames) != EndpointStatus::Ready) return;
        }
    };
    // The reader is the ring's other side: a thread of its own, started before measurement.
    std::atomic<bool> stop{false}, running{false}; std::atomic<unsigned> read{0};
    std::thread reader([&] {
        running.store(true); unsigned total = 0;
        for (;;) {
            const bool last = stop.load(std::memory_order_acquire);
            const auto n = capture.Read(std::span<float>{recorded + total, std::size(recorded) - total});
            total += n; read.store(total, std::memory_order_release);
            if (last && !n) break;
            if (!n) Sleep(1);
        }
    });
    while (!running.load()) Sleep(1);
    allocation_probe::Begin();
    CHECK(capture.Start() == EndpointStatus::Ready); CHECK(capture.Start() == EndpointStatus::InvalidArgument);
    CHECK(bounded.Start() == EndpointStatus::Ready);
    feed(); CHECK(sink.Start() == EndpointStatus::Ready);
    const auto end = GetTickCount64() + 900;
    while (GetTickCount64() < end) {
        feed(); (void)sink.Pump();
        auto pumped = capture.Pump(); CHECK(pumped == EndpointStatus::Ready || pumped == EndpointStatus::WouldBlock);
        pumped = bounded.Pump(); CHECK(pumped == EndpointStatus::Ready || pumped == EndpointStatus::WouldBlock);
        Sleep(1);
    }
    CHECK(sink.Stop() == EndpointStatus::Ready);
    (void)capture.Pump(); (void)bounded.Pump();
    CHECK(capture.Stop() == EndpointStatus::Ready); CHECK(bounded.Stop() == EndpointStatus::Ready);
    stop.store(true, std::memory_order_release);
    const auto allocations = allocation_probe::End();
    reader.join();
    const auto& s = capture.Stats(); const auto& t = bounded.Stats();
    const unsigned total = read.load(), rate = capture.SampleRate();
    // The tone against two frequencies that were not played, over everything captured after the first 100 ms.
    const unsigned skip = (std::min)(total, rate / 10);
    const double tone = Power(recorded + skip, total - skip, 1000, rate), other = (std::max)(Power(recorded + skip, total - skip, 2500, rate), Power(recorded + skip, total - skip, 330, rate));
    std::printf("Loopback capture: endpoint %u Hz %u ch -> mono %u Hz (%s); %llu frames in %u packets (%u silent, %u discontinuities), %llu dropped, %u read by the reader thread\n",
                capture.EndpointRate(), capture.EndpointChannels(), rate, capture.Converting() ? "converted by Windows" : "endpoint format, averaged here",
                static_cast<unsigned long long>(s.frames), s.packets, s.silent, s.discontinuities, static_cast<unsigned long long>(s.dropped), total);
    std::printf("Tone: 1 kHz power %.3e, strongest other %.3e (%.1f dB above); small ring: %llu kept, %llu dropped; allocations %llu\n", tone, other,
                other > 0 ? 10 * std::log10(tone / other) : 99.0, static_cast<unsigned long long>(t.frames), static_cast<unsigned long long>(t.dropped),
                static_cast<unsigned long long>(allocations));
    CHECK(s.frames >= rate / 2 && s.dropped == 0 && total == s.frames && s.first_qpc_100ns > 0);
    CHECK(t.frames == 1024 && t.dropped > 0 && t.frames + t.dropped >= rate / 2); // Bounded: the newest frames go.
    CHECK(allocations == 0);
    // What stayed in the small ring is the oldest audio, intact.
    static float kept[1024]; CHECK(bounded.Read(kept) == 1024 && bounded.Read(kept) == 0);
    // A silent endpoint (EndpointSilent): the ring, its reader and the bounded ring were tested above, the endpoint's audio
    // cannot be.
    const bool heard = tone > 1e-4 && tone > 10 * other;
    if (EndpointSilent(s, phase, heard)) {
        std::printf("SKIP the endpoint's loopback delivered silent-flagged packets (%u of %u) while %llu tone frames were rendered: the "
                    "default output device does not loop back (headphone endpoints have been seen to); the ring checks above passed (%d failures)\n",
                    s.silent, s.packets, static_cast<unsigned long long>(phase), failures);
        capture.Close(); bounded.Close(); sink.Close();
        return failures ? 1 : 77;
    }
    CHECK(heard); // The amplitude 0.05 tone: about 5e-4 measured, 24 to 63 dB above the rest on a quiet system.
    capture.Close(); bounded.Close(); sink.Close();
    std::printf("Capture: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
// The detector on the real capture ring. What is played: 0.7 s of silence, a 0.45 s "word" (a 150 Hz harmonic vowel
// with soft edges), then silence. The capture's clock and latency are the endpoint's, so the word's position is not
// asserted, only that it arrives as one utterance of about its length. A loopback hears everything the machine
// plays: when the half second before the word or the time after it is not silent (another program is playing), the
// test skips.
struct Heard {
    unsigned starts = 0, ends = 0; std::uint64_t frames = 0;
    static void Sink(void* context, const audio::SpeechSlice& slice) noexcept {
        auto& h = *static_cast<Heard*>(context);
        if (slice.state == audio::VadState::SpeechStart) ++h.starts;
        if (slice.state == audio::VadState::SpeechEnd) ++h.ends;
        h.frames += slice.frames;
    }
};
int TestVad() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
    Win32WasapiAudioSink sink;
    if (sink.Prepare(arena) != EndpointStatus::Ready) { std::printf("SKIP no render endpoint (%08lx)\n", static_cast<unsigned long>(sink.LastError())); return 77; }
    Win32WasapiCapture capture;
    bool process = false;
    CHECK(PrepareLoopback(capture, arena, CaptureOptions{}, process) == EndpointStatus::Ready);
    std::printf("Loopback source: %s\n", process ? "this process (process loopback)" : "the default endpoint");
    audio::MicroVad vad; audio::VadOptions vad_options; vad_options.sample_rate = capture.SampleRate();
    CHECK(vad.Configure(arena, vad_options));
    if (failures) return 1;
    CHECK(arena.Seal());
    const auto format = sink.Format();
    const double word_begin = 0.7, word_end = 1.15;
    std::uint64_t phase = 0;
    const auto feed = [&] {
        for (;;) {
            AudioWriteSpan span{};
            if (sink.TryAcquire(1024, span) != EndpointStatus::Ready) return;
            auto* out = reinterpret_cast<float*>(span.data);
            for (std::uint32_t i = 0; i < span.frames; ++i, ++phase) {
                const double t = static_cast<double>(phase) / format.sample_rate; double v = 0;
                if (t >= word_begin && t < word_end) {
                    const double edge = (std::min)({1.0, (t - word_begin) / 0.02, (word_end - t) / 0.04});
                    for (unsigned k = 1; k <= 12; ++k) v += std::sin(2 * kPi * 150.0 * k * t + 0.7 * k) / k;
                    v *= 0.04 * edge;
                }
                for (unsigned c = 0; c < format.channels; ++c) out[i * format.channels + c] = static_cast<float>(v);
            }
            if (sink.Commit(span.lease, span.frames) != EndpointStatus::Ready) return;
        }
    };
    // The reader thread owns the detector: it drains the ring into it, with no buffer of its own.
    std::atomic<bool> stop{false}, running{false}; Heard heard; std::uint64_t taken = 0;
    float loudest_around = -120; // The loudest window of the first half second and after 1.65 s, where only silence is played.
    std::thread reader([&] {
        running.store(true);
        for (;;) {
            const bool last = stop.load(std::memory_order_acquire);
            const auto n = vad.Drain(capture, &Heard::Sink, &heard);
            taken += n;
            if (n && (vad.Stats().windows <= 50 || vad.Stats().windows > 165)) loudest_around = (std::max)(loudest_around, vad.Features().level_db);
            if (last && !n) break;
            if (!n) Sleep(1);
        }
    });
    while (!running.load()) Sleep(1);
    allocation_probe::Begin();
    CHECK(capture.Start() == EndpointStatus::Ready);
    feed(); CHECK(sink.Start() == EndpointStatus::Ready);
    const auto end = GetTickCount64() + 2100;
    while (GetTickCount64() < end) {
        feed(); (void)sink.Pump();
        const auto pumped = capture.Pump(); CHECK(pumped == EndpointStatus::Ready || pumped == EndpointStatus::WouldBlock);
        Sleep(1);
    }
    CHECK(sink.Stop() == EndpointStatus::Ready);
    (void)capture.Pump(); CHECK(capture.Stop() == EndpointStatus::Ready);
    stop.store(true, std::memory_order_release);
    reader.join();
    const auto allocations = allocation_probe::End();
    const auto& s = capture.Stats(); const auto& v = vad.Stats(); const auto& u = vad.Last();
    const double rate = capture.SampleRate();
    if (EndpointSilent(s, phase, v.utterances != 0)) { // As in TestLoopback: the endpoint does not loop back.
        std::printf("SKIP the endpoint's loopback delivered silent-flagged packets (%u of %u) while %llu frames were rendered: the default "
                    "output device does not loop back; the detector was not tested on captured sound\n", s.silent, s.packets, static_cast<unsigned long long>(phase));
        CHECK(taken == s.frames && s.dropped == 0 && allocations == 0);
        capture.Close(); sink.Close(); return failures ? 1 : 77;
    }
    if (loudest_around > -60) {
        std::printf("SKIP other audio is playing on the endpoint (%.1f dBFS around the word; %u utterances heard)\n", static_cast<double>(loudest_around), v.utterances);
        capture.Close(); sink.Close(); return failures || allocations ? 1 : 77;
    }
    std::printf("Capture VAD: %llu frames at %u Hz drained from the ring (%llu dropped); %u windows, %u utterances; the word: %.3f s active, %.3f s delivered with pre-roll and hangover; %.1f%% of the capture delivered; allocations %llu\n",
                static_cast<unsigned long long>(taken), capture.SampleRate(), static_cast<unsigned long long>(s.dropped), v.windows, v.utterances,
                static_cast<double>(u.end_frame - u.onset_frame) / rate, static_cast<double>(u.delivered) / rate,
                v.frames ? 100.0 * static_cast<double>(v.delivered) / static_cast<double>(v.frames) : 0.0, static_cast<unsigned long long>(allocations));
    CHECK(taken == s.frames && s.dropped == 0 && taken >= capture.SampleRate() * 3 / 2);
    // One utterance, as long as the word (0.45 s) within 60 ms, delivered with 100 ms before and 250 ms after it.
    CHECK(v.utterances == 1 && heard.starts == 1 && heard.ends == 1 && !vad.InUtterance() && !u.truncated);
    const double length = static_cast<double>(u.end_frame - u.onset_frame) / rate;
    CHECK(length > 0.39 && length < 0.51);
    CHECK(heard.frames == u.delivered && v.delivered == u.delivered && static_cast<double>(u.delivered) / rate > length + 0.30 && static_cast<double>(u.delivered) / rate < length + 0.40);
    CHECK(allocations == 0);
    capture.Close(); sink.Close();
    std::printf("Capture VAD: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
}
int main(int argc, char** argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    const int result = argc > 1 && std::strcmp(argv[1], "microphone") == 0 ? TestMicrophone() :
                       argc > 1 && std::strcmp(argv[1], "vad") == 0 ? TestVad() : TestLoopback();
    CoUninitialize();
    return result;
}
