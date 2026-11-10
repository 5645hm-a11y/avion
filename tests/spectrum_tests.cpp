// Music mode's analysis (audio::SpectrumAnalyser; docs/audio-dsp.md, "Music mode"): a full-scale sine reads 0 dB in its
// band and nothing far from it, two tones make two peaks, silence reads nothing, the waveform envelope and the level
// follow the signal, blocks are stamped with the media time of their first frame across any feeding pattern, Reset
// drops a partial block, and nothing allocates after Configure.
#include "media/audio/SpectrumAnalyser.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>
using media::audio::AudioPicture;
using media::audio::SpectrumAnalyser;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr double kPi = 3.14159265358979323846;
std::vector<float> Tones(std::initializer_list<double> hz, double amplitude, unsigned rate, unsigned channels, unsigned frames) {
    std::vector<float> v(static_cast<std::size_t>(frames) * channels);
    for (unsigned i = 0; i < frames; ++i) {
        double s = 0; for (const double f : hz) s += amplitude * std::sin(2 * kPi * f * i / rate);
        for (unsigned c = 0; c < channels; ++c) v[static_cast<std::size_t>(i) * channels + c] = static_cast<float>(s);
    }
    return v;
}
// Feeds everything in pieces of `piece` frames; returns the pictures completed, in order.
std::vector<AudioPicture> Run(SpectrumAnalyser& a, const std::vector<float>& pcm, unsigned channels, unsigned rate, unsigned piece, std::int64_t origin) {
    std::vector<AudioPicture> out; out.reserve(pcm.size() / channels / SpectrumAnalyser::kBlock + 1);
    const unsigned frames = static_cast<unsigned>(pcm.size() / channels);
    unsigned done = 0;
    while (done < frames) {
        const unsigned n = std::min(piece, frames - done);
        unsigned taken = 0;
        while (taken < n) {
            const auto media = origin + static_cast<std::int64_t>(done + taken) * 10000000 / rate;
            taken += a.Feed(pcm.data() + static_cast<std::size_t>(done + taken) * channels, n - taken, media, 7);
            if (a.Ready()) out.push_back(a.Take());
        }
        done += n;
    }
    return out;
}
unsigned Loudest(const AudioPicture& p) { unsigned best = 0; for (unsigned b = 1; b < AudioPicture::kBands; ++b) if (p.bands[b] > p.bands[best]) best = b; return best; }
void Sines() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    for (const unsigned rate : {48000u, 44100u}) {
        SpectrumAnalyser a; CHECK(a.Configure(arena, rate, 2));
        for (const unsigned band : {4u, 12u, 20u, 28u}) {
            const double hz = a.BandCentre(band);
            a.Reset();
            const auto pictures = Run(a, Tones({hz}, 1.0, rate, 2, 8 * SpectrumAnalyser::kBlock), 2, rate, 480, 0);
            CHECK(pictures.size() == 8 && a.BandOf(hz) == band);
            if (pictures.size() != 8) continue;
            const auto& p = pictures[4];
            // Its band at 0 dB (235, within 1 dB: 3.3 codes), the loudest when the band has bins of its own, and every band two
            // octaves or more away below -40 dB (104).
            unsigned far = 0;
            for (unsigned b = 0; b < AudioPicture::kBands; ++b) {
                const double centre = a.BandCentre(b);
                if ((centre > 4 * hz || centre < hz / 4) && p.bands[b] > far) far = p.bands[b];
            }
            std::printf("%u Hz: %.1f Hz sine in band %u (%.0f-%.0f Hz, %s): %u, loudest band %u, farthest bands at most %u, level %.3f, peak %.3f\n", rate, hz, band,
                        a.BandEdge(band), a.BandEdge(band + 1), a.Resolved(band) ? "resolved" : "three bins", p.bands[band], Loudest(p), far,
                        static_cast<double>(p.level), static_cast<double>(p.peak));
            CHECK(std::abs(p.bands[band] - 235) <= 3 && (!a.Resolved(band) || Loudest(p) == band) && far < 104);
            // A full-scale sine: RMS -3 dBFS (level 0.95), peak 1, the envelope at +-127 where a column holds a crest
            // (above 23 Hz a 1024-frame block holds one).
            CHECK(std::fabs(p.level - 0.95f) < 0.01f && p.peak > 0.99f);
            int high = -128, low = 127; for (unsigned c = 0; c < AudioPicture::kColumns; ++c) { high = std::max<int>(high, p.high[c]); low = std::min<int>(low, p.low[c]); }
            CHECK(high >= 126 && low <= -126);
        }
    }
}
void TwoTonesAndSilence() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    SpectrumAnalyser a; CHECK(a.Configure(arena, 48000, 1));
    const unsigned b1 = 10, b2 = 22, middle = 16;
    const auto two = Run(a, Tones({a.BandCentre(b1), a.BandCentre(b2)}, 0.25, 48000, 1, 4 * SpectrumAnalyser::kBlock), 1, 48000, 1024, 0);
    CHECK(two.size() == 4);
    if (two.size() == 4) {
        const auto& p = two[2];
        // -12 dB each: 60 dB above the floor, 196 codes. Between them (2.4 octaves from either), far less.
        std::printf("Two tones: bands %u and %u at %u and %u, band %u between them at %u\n", b1, b2, p.bands[b1], p.bands[b2], middle, p.bands[middle]);
        CHECK(std::abs(p.bands[b1] - 196) <= 3 && std::abs(p.bands[b2] - 196) <= 3 && p.bands[middle] < 100);
    }
    a.Reset();
    const auto silent = Run(a, std::vector<float>(2 * SpectrumAnalyser::kBlock, 0.0f), 1, 48000, 333, 0);
    CHECK(silent.size() == 2);
    bool nothing = true;
    for (const auto& p : silent) { for (const auto b : p.bands) nothing = nothing && b == 0; nothing = nothing && p.level == 0 && p.peak == 0; }
    CHECK(nothing);
    // Reset forgets the previous block: the first block after it is analysed with silence before it.
    a.Reset();
    const auto after = Run(a, Tones({a.BandCentre(b1)}, 1.0, 48000, 1, SpectrumAnalyser::kBlock), 1, 48000, 1024, 0);
    a.Reset();
    const auto quiet = Run(a, std::vector<float>(SpectrumAnalyser::kBlock, 0.0f), 1, 48000, 1024, 0);
    CHECK(after.size() == 1 && quiet.size() == 1 && after[0].bands[b1] > 200 && quiet[0].bands[b1] == 0);
}
// Media stamps: each block carries the media time of its first frame, whatever the pieces it was fed in; Reset drops the
// partial block, and the next block starts with the next frame fed.
void Stamps() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    SpectrumAnalyser a; CHECK(a.Configure(arena, 48000, 2));
    const auto tone = Tones({440.0}, 0.5, 48000, 2, 10 * SpectrumAnalyser::kBlock);
    for (const unsigned piece : {1u, 7u, 480u, 1024u, 4096u}) {
        a.Reset();
        const auto pictures = Run(a, tone, 2, 48000, piece, 50000000);
        bool stamped = pictures.size() == 10;
        for (std::size_t i = 0; stamped && i < pictures.size(); ++i)
            stamped = pictures[i].media_100ns == 50000000 + static_cast<std::int64_t>(i * SpectrumAnalyser::kBlock) * 10000000 / 48000 && pictures[i].epoch == 7;
        CHECK(stamped);
    }
    a.Reset();
    CHECK(a.Feed(tone.data(), 600, 0, 1) == 600 && !a.Ready());
    a.Reset();
    CHECK(a.Feed(tone.data(), 1000, 123, 2) == 1000 && !a.Ready() && a.Feed(tone.data(), 100, 456, 2) == 24 && a.Ready());
    CHECK(a.Feed(tone.data(), 100, 789, 2) == 0); // The picture waits to be taken.
    const auto& p = a.Take(); CHECK(p.media_100ns == 123 && p.epoch == 2 && !a.Ready());
}
void Allocations() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    SpectrumAnalyser a; CHECK(a.Configure(arena, 48000, 6));
    CHECK(!a.Configure(arena, 48000, 6)); // Once.
    SpectrumAnalyser b; CHECK(!b.Configure(arena, 4000, 2) && !b.Configure(arena, 48000, 9));
    const auto tone = Tones({1000.0}, 0.5, 48000, 6, 16 * SpectrumAnalyser::kBlock);
    unsigned pictures = 0;
    allocation_probe::Begin();
    for (unsigned done = 0; done < 16 * SpectrumAnalyser::kBlock;) {
        done += a.Feed(tone.data() + static_cast<std::size_t>(done) * 6, 700, done, 1);
        if (a.Ready()) { (void)a.Take(); ++pictures; }
    }
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0 && pictures == 16);
    CHECK(arena.Seal());
    SpectrumAnalyser sealed; CHECK(!sealed.Configure(arena, 48000, 2)); // Setup only.
}
}
int main() {
    std::puts("Spectrum tests"); // The first output allocates stdout's buffer (seen by the probe in MSVC Debug): before any probe.
    Sines(); TwoTonesAndSilence(); Stamps(); Allocations();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
