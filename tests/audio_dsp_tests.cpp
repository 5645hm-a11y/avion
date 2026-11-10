#include <vector>
#include "media/audio/DspProcessor.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using media::audio::DspProcessor;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 30) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr double pi = 3.14159265358979323846;
constexpr unsigned rate = 48000;
float Tone(unsigned frame, double hz, float amplitude, unsigned sample_rate = rate) {
    return amplitude * static_cast<float>(std::sin(2 * pi * hz * frame / sample_rate));
}
void ConfigureAndBypass() {
    DspProcessor dsp;
    float untouched = 0.25f;
    dsp.Process(&untouched, 1); CHECK(untouched == 0.25f);
    for (unsigned channels : {1u, 2u, 6u, 8u}) {
        for (unsigned hz : {8000u, 44100u, 48000u, 96000u, 192000u}) {
            CHECK(dsp.Configure(hz, channels)); CHECK(dsp.Channels() == channels);
            std::array<float, 64> input{}, original{};
            // Even over-range values, signed zeros and NaN payloads must survive disabled DSP.
            constexpr std::uint32_t bits[]{0u, 0x80000000u, 0x3e800000u, 0xbf000000u, 0x40000000u, 0x7fc12345u, 0x7f800000u, 0xff800000u};
            for (unsigned i = 0; i < input.size(); ++i) input[i] = std::bit_cast<float>(bits[i % 8]);
            original = input; dsp.Process(input.data(), 8);
            CHECK(std::memcmp(input.data(), original.data(), sizeof(input)) == 0);
            CHECK(!dsp.Active()); CHECK(dsp.Statistics().frames == 0);
        }
    }
    CHECK(!dsp.Configure(7999, 2)); CHECK(!dsp.Configure(192001, 2));
    for (unsigned ch : {0u, 3u, 4u, 5u, 7u, 9u}) CHECK(!dsp.Configure(rate, ch));
    CHECK(dsp.Configure(rate, 1)); dsp.SetSpatial(true); CHECK(!dsp.Active());
    dsp.Process(nullptr, 99); dsp.Process(&untouched, 0);
}
void Dialogue() {
    float previous_gain = 1;
    for (unsigned level : {1u, 2u, 3u}) {
        DspProcessor dsp; CHECK(dsp.Configure(rate, 2)); dsp.SetClarity(level);
        double in_energy = 0, out_energy = 0;
        for (unsigned f = 0; f < 3 * rate; ++f) {
            const float v = Tone(f, 1500, 0.02f); float pcm[]{v, v}; dsp.Process(pcm, 1);
            if (f >= 2 * rate) { in_energy += v * v; out_energy += pcm[0] * pcm[0]; }
            CHECK(pcm[0] == pcm[1]);
        }
        const float gain = static_cast<float>(std::sqrt(out_energy / in_energy));
        const float maximum = level == 1 ? 4.0f : level == 2 ? 7.0f : 10.0f;
        CHECK(dsp.Statistics().max_boost_db > maximum - 0.25f);
        CHECK(dsp.Statistics().max_boost_db <= maximum + 0.01f);
        CHECK(std::fabs(20 * std::log10(gain) - maximum) < 0.3f);
        CHECK(gain > previous_gain); previous_gain = gain;
    }
    // Silence, centred broadband white noise, side-only speech and out-of-band tones.
    for (unsigned kind = 0; kind < 5; ++kind) {
        DspProcessor dsp; CHECK(dsp.Configure(rate, 2)); dsp.SetClarity(3);
        std::uint32_t random = 12345;
        double input_energy = 0, output_energy = 0;
        for (unsigned f = 0; f < 2 * rate; ++f) {
            random = random * 1664525u + 1013904223u;
            const float noise = (static_cast<float>(random >> 8) / 8388608.0f - 1) * 0.02f;
            const float v = kind == 0 ? 0 : kind == 1 ? noise : Tone(f, kind == 2 ? 1500 : kind == 3 ? 80 : 10000, 0.02f);
            float pcm[]{v, kind == 2 ? -v : v}; dsp.Process(pcm, 1);
            if (f >= rate) { input_energy += v * v; output_energy += pcm[0] * pcm[0]; }
        }
        CHECK(dsp.Statistics().max_boost_db < 0.5f);
        CHECK(output_energy <= input_energy * 1.10 + 1e-25);
    }
    // In surround layouts, the centre is the dialogue carrier; other speakers stay untouched at low levels.
    for (unsigned channels : {6u, 8u}) {
        DspProcessor dsp; CHECK(dsp.Configure(rate, channels)); dsp.SetClarity(3);
        for (unsigned f = 0; f < 2 * rate; ++f) {
            float pcm[8]{}; pcm[0] = 0.001f; pcm[2] = Tone(f, 1500, 0.02f);
            dsp.Process(pcm, 1); CHECK(std::fabs(pcm[0] - 0.001f) < 1e-7f);
        }
        CHECK(dsp.Statistics().max_boost_db > 9.5f);
    }
}
void ClippingAndCompression() {
    // The -16 dBFS threshold has an 8 dB knee; level 3 uses a 4:1 ratio.
    // A 10 kHz carrier avoids dialogue gain and makes detector ripple negligible.
    for (float db : {-24.0f, -16.0f, -6.0f}) {
        DspProcessor compressor; CHECK(compressor.Configure(rate, 1)); compressor.SetClarity(3);
        const float amplitude = std::pow(10.0f, db / 20);
        double before = 0, after = 0;
        for (unsigned f = 0; f < 2 * rate; ++f) {
            const float original = Tone(f, 10000, amplitude); float sample = original;
            compressor.Process(&sample, 1);
            if (f >= rate) { before += original * original; after += sample * sample; }
        }
        const double expected = db == -24 ? 0 : db == -16 ? -0.75 : -7.5;
        CHECK(std::fabs(10 * std::log10(after / before) - expected) < 0.15);
    }
    for (unsigned channels : {1u, 2u, 6u, 8u}) {
        for (unsigned mode : {1u, 2u, 3u}) {
            DspProcessor dsp; CHECK(dsp.Configure(rate, channels));
            dsp.SetClarity(mode == 2 ? 0 : 3); dsp.SetSpatial(mode != 1);
            if (!dsp.Active()) continue;
            for (unsigned f = 0; f < rate; ++f) {
                float pcm[8]{};
                for (unsigned c = 0; c < channels; ++c) pcm[c] = ((f + c) % 2 ? -1.0f : 1.0f) * (f < 100 ? 16.0f : 2.0f);
                dsp.Process(pcm, 1);
                for (unsigned c = 0; c < channels; ++c) CHECK(std::isfinite(pcm[c]) && std::fabs(pcm[c]) <= 0.99999994f);
            }
            if (mode != 2) CHECK(dsp.Statistics().min_gain_db < -12);
        }
    }
    // A corrupt sample must not poison later valid PCM.
    DspProcessor dsp; CHECK(dsp.Configure(rate, 2)); dsp.SetClarity(3); dsp.SetSpatial(true);
    float invalid[]{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
    dsp.Process(invalid, 1);
    float peak = 0;
    for (unsigned f = 0; f < rate; ++f) {
        float pcm[]{Tone(f, 1500, 0.02f), Tone(f, 1500, 0.02f)}; dsp.Process(pcm, 1);
        CHECK(std::isfinite(pcm[0]) && std::isfinite(pcm[1])); peak = std::max(peak, std::fabs(pcm[0]));
    }
    CHECK(peak > 0.005f); // Spatial comb filtering can attenuate a centred 1.5 kHz tone.
}
void Spatial() {
    constexpr float positions51[]{-30, 30, 0, 0, -110, 110};
    constexpr float positions71[]{-30, 30, 0, 0, -150, 150, -90, 90};
    for (unsigned hz : {8000u, 44100u, 48000u, 96000u, 192000u}) {
        for (unsigned channels : {2u, 6u, 8u}) {
            double energies[8][2]{};
            for (unsigned source = 0; source < channels; ++source) {
                DspProcessor dsp; CHECK(dsp.Configure(hz, channels)); dsp.SetSpatial(true);
                // Finish the 30 ms enable ramp before measuring the impulse response.
                std::array<float, 8 * 256> zero{};
                for (unsigned f = 0; f < hz / 20 + 256; f += 256) dsp.Process(zero.data(), 256);
                double sum[2]{}, moment[2]{};
                for (unsigned f = 0; f < 2048; ++f) {
                    float pcm[8]{}; if (f == 0) pcm[source] = 0.1f;
                    dsp.Process(pcm, 1);
                    for (unsigned ear = 0; ear < 2; ++ear) {
                        sum[ear] += pcm[ear]; moment[ear] += f * static_cast<double>(pcm[ear]);
                        energies[source][ear] += pcm[ear] * pcm[ear];
                    }
                    for (unsigned c = 2; c < channels; ++c) CHECK(pcm[c] == 0);
                }
                const float azimuth = channels == 8 ? positions71[source] : positions51[source];
                const bool lfe = channels > 2 && source == 3;
                const double weight = source < 2 ? 1 : lfe ? 0.5 : std::sqrt(0.5);
                float dl = 0, dr = 0, al = 0, ar = 0;
                DspProcessor::EarModel(azimuth, dl, dr, al, ar);
                // Independent Woodworth ITD at the front speaker angle: a/c * (sin(theta) + theta).
                if (source < 2) CHECK(std::fabs(std::fabs(dr - dl) - (0.0875 / 343) * (0.5 + pi / 6)) < 1e-9);
                for (unsigned ear = 0; ear < 2; ++ear) {
                    // Unity DC head filter and unit-sum interpolation preserve each speaker's declared mix weight.
                    CHECK(std::fabs(sum[ear] - 0.05 * weight) < 2e-6);
                    const double delay = ((ear ? dr : dl) - std::min(dl, dr)) * hz;
                    const double shadow_delay = (1 - (ear ? ar : al)) * hz / (2 * (343.0 / 0.0875));
                    const double pinna = std::fabs(azimuth) > 90 ? 0.5 / std::expm1(2 * pi * 4500 / hz) : 0;
                    CHECK(std::fabs(moment[ear] / sum[ear] - (lfe ? 0 : delay + shadow_delay + pinna)) < 0.02);
                }
                if (azimuth < 0) CHECK(energies[source][0] > energies[source][1]);
                if (azimuth > 0) CHECK(energies[source][1] > energies[source][0]);
            }
            // Mirrored speakers exchange ears with identical energy; centre/LFE are symmetric.
            for (unsigned left : {0u, 4u, 6u}) if (left + 1 < channels) {
                CHECK(std::fabs(energies[left][0] - energies[left + 1][1]) < 1e-9);
                CHECK(std::fabs(energies[left][1] - energies[left + 1][0]) < 1e-9);
            }
            if (channels > 2) for (unsigned c : {2u, 3u}) CHECK(std::fabs(energies[c][0] - energies[c][1]) < 1e-9);
        }
    }
    // A centred low-frequency phantom keeps its stereo level (no unintended 6 dB summation gain).
    DspProcessor dsp; CHECK(dsp.Configure(rate, 2)); dsp.SetSpatial(true);
    double original = 0, rendered = 0;
    for (unsigned f = 0; f < rate; ++f) {
        const float v = Tone(f, 100, 0.1f); float pcm[]{v, v}; dsp.Process(pcm, 1);
        if (f > rate / 2) { original += v * v; rendered += pcm[0] * pcm[0]; }
        CHECK(pcm[0] == pcm[1]);
    }
    CHECK(rendered / original > 0.95 && rendered / original < 1.05);
}
void ResetAndBlockInvariance() {
    for (unsigned channels : {1u, 2u, 6u, 8u}) {
        DspProcessor whole, split;
        CHECK(whole.Configure(rate, channels)); CHECK(split.Configure(rate, channels));
        whole.SetClarity(3); split.SetClarity(3); whole.SetSpatial(true); split.SetSpatial(true);
        std::array<float, 8 * 4096> a{}, b{};
        for (unsigned pass = 0; pass < 5; ++pass) {
            if (pass == 2) { whole.SetClarity(0); split.SetClarity(0); whole.SetSpatial(false); split.SetSpatial(false); }
            if (pass == 4) {
                whole.SetClarity(2); split.SetClarity(2); whole.SetSpatial(true); split.SetSpatial(true);
                whole.Reset(); split.Reset();
            }
            for (unsigned f = 0; f < 4096; ++f) for (unsigned c = 0; c < channels; ++c)
                a[f * channels + c] = Tone(f + pass * 4096, 700 + c * 113, 0.1f);
            b = a; whole.Process(a.data(), 4096);
            for (unsigned f = 0; f < 4096;) {
                const unsigned count = std::min(4096 - f, 1 + (f * 7) % 131);
                split.Process(b.data() + f * channels, count); f += count;
            }
            CHECK(std::memcmp(a.data(), b.data(), sizeof(a)) == 0);
            if (pass == 3) { CHECK(!whole.Active()); CHECK(!split.Active()); }
        }
        // Reset retains settings, but its next output must match a freshly configured processor.
        DspProcessor fresh; CHECK(fresh.Configure(rate, channels)); fresh.SetClarity(2); fresh.SetSpatial(true);
        whole.Reset(); a.fill(0.03f); b = a; whole.Process(a.data(), 4096); fresh.Process(b.data(), 4096);
        CHECK(std::memcmp(a.data(), b.data(), sizeof(a)) == 0);
        whole.SetClarity(0); whole.SetSpatial(false); whole.Process(a.data(), 4096);
        a.fill(-0.0f); b = a; whole.Process(a.data(), 4096);
        CHECK(!whole.Active()); CHECK(std::memcmp(a.data(), b.data(), sizeof(a)) == 0);
    }
}
// Stereo fold-down of surround input: ITU weights with spatial off, the verified binaural render with spatial on,
// channels 2+ always silent, and a toggle between the two that never steps.
void FoldDown() {
    DspProcessor none; CHECK(!none.Configure(rate, 2, true)); CHECK(!none.Configure(rate, 1, true));
    for (unsigned channels : {6u, 8u}) {
        DspProcessor dsp; CHECK(dsp.Configure(rate, channels, true)); CHECK(dsp.FoldDown() && dsp.Active()); // Never a passthrough.
        // DC on one channel at a time: exact downmix weights, other outputs exactly zero.
        const float fronts = 0.5f, centre = 0.5f * 0.70710678f;
        for (unsigned source = 0; source < channels; ++source) {
            dsp.Reset();
            float pcm[8]{}; float left = 0, right = 0;
            for (unsigned f = 0; f < 256; ++f) { std::fill(pcm, pcm + 8, 0.0f); pcm[source] = 0.25f; dsp.Process(pcm, 1); }
            DspProcessor::DownmixWeights(channels, source, left, right);
            CHECK(pcm[0] == 0.25f * left && pcm[1] == 0.25f * right);
            for (unsigned c = 2; c < channels; ++c) CHECK(pcm[c] == 0);
            const float expected_left = source == 0 ? fronts : source == 2 ? centre : source == 3 ? 0 : source % 2 == 0 && source >= 4 ? centre : 0;
            CHECK(left == expected_left); // FL 0.5, FC 0.354 (both), LFE 0, left surround/side/back 0.354.
            if (source == 2) CHECK(right == centre);
        }
        // Spatial on: once the ramp completes, the ears equal the non-fold binaural render (whose speaker positions,
        // delays and head shadow Spatial() verifies) for an impulse on every speaker.
        for (unsigned source = 0; source < channels; ++source) {
            DspProcessor fold, plain; CHECK(fold.Configure(rate, channels, true) && plain.Configure(rate, channels));
            fold.SetSpatial(true); plain.SetSpatial(true);
            std::array<float, 8 * 256> a{}, b{};
            for (unsigned f = 0; f < rate / 20 + 256; f += 256) { a.fill(0); b.fill(0); fold.Process(a.data(), 256); plain.Process(b.data(), 256); }
            double worst = 0;
            for (unsigned f = 0; f < 1024; ++f) {
                float x[8]{}, y[8]{}; if (f == 0) x[source] = y[source] = 0.1f;
                fold.Process(x, 1); plain.Process(y, 1);
                worst = std::max({worst, static_cast<double>(std::fabs(x[0] - y[0])), static_cast<double>(std::fabs(x[1] - y[1]))});
                for (unsigned c = 2; c < channels; ++c) CHECK(x[c] == 0);
            }
            CHECK(worst < 1e-7);
        }
        // Toggle mid-programme: a different 997 Hz phase per channel. The largest sample-to-sample step during either
        // 30 ms crossfade stays within the steady states' own largest step (a hard switch would jump by up to ~0.5).
        DspProcessor toggle; CHECK(toggle.Configure(rate, channels, true));
        float previous[2]{}, steady = 0, crossfade = 0;
        for (unsigned f = 0; f < rate; ++f) {
            if (f == rate / 4) toggle.SetSpatial(true);
            if (f == rate / 2 + rate / 8) toggle.SetSpatial(false);
            float x[8]{};
            for (unsigned c = 0; c < channels; ++c) x[c] = c == 3 ? 0 : 0.3f * static_cast<float>(std::sin(2 * pi * 997 * f / rate + c));
            toggle.Process(x, 1);
            const bool fading = (f >= rate / 4 && f < rate / 4 + rate / 20) || (f >= rate / 2 + rate / 8 && f < rate / 2 + rate / 8 + rate / 20);
            if (f > 64) for (unsigned e = 0; e < 2; ++e) {
                const float step = std::fabs(x[e] - previous[e]);
                (fading ? crossfade : steady) = std::max(fading ? crossfade : steady, step);
            }
            previous[0] = x[0]; previous[1] = x[1];
            for (unsigned c = 2; c < channels; ++c) CHECK(x[c] == 0);
        }
        std::printf("Fold-down %u ch: largest step steady %.4f, during crossfades %.4f\n", channels, static_cast<double>(steady), static_cast<double>(crossfade));
        CHECK(steady > 0 && crossfade <= steady * 1.05f);
        // Clipping: full-scale coherent surround stays bounded.
        DspProcessor loud; CHECK(loud.Configure(rate, channels, true));
        for (unsigned f = 0; f < 4800; ++f) { float x[8]; std::fill(x, x + 8, 1.0f); loud.Process(x, 1); CHECK(std::fabs(x[0]) < 1 && std::fabs(x[1]) < 1); }
    }
    // Passthrough (no fold-down) toggle: the ears ramp into channels 0/1 while channels 2+ fade out, and back; no
    // channel steps by more than the steady states do.
    for (unsigned channels : {6u, 8u}) {
        DspProcessor toggle; CHECK(toggle.Configure(rate, channels));
        float previous[8]{}, steady[8]{}, crossfade[8]{};
        for (unsigned f = 0; f < rate; ++f) {
            if (f == rate / 4) toggle.SetSpatial(true);
            if (f == rate / 2 + rate / 8) toggle.SetSpatial(false);
            float x[8]{};
            for (unsigned c = 0; c < channels; ++c) x[c] = c == 3 ? 0 : 0.3f * static_cast<float>(std::sin(2 * pi * 997 * f / rate + c));
            toggle.Process(x, 1);
            const bool fading = (f >= rate / 4 && f < rate / 4 + rate / 20) || (f >= rate / 2 + rate / 8 && f < rate / 2 + rate / 8 + rate / 20);
            if (f > 64) for (unsigned c = 0; c < channels; ++c) { auto& worst = fading ? crossfade[c] : steady[c]; worst = std::max(worst, std::fabs(x[c] - previous[c])); }
            std::copy(x, x + 8, previous);
        }
        for (unsigned c = 0; c < channels; ++c) if (c != 3) CHECK(steady[c] > 0 && crossfade[c] <= steady[c] * 1.05f);
    }
    // Allocation-free, like the rest of the processor.
    DspProcessor dsp; CHECK(dsp.Configure(rate, 8, true)); dsp.SetSpatial(true);
    std::array<float, 8 * 512> block{};
    allocation_probe::Begin();
    for (unsigned i = 0; i < 200; ++i) { dsp.SetSpatial(i % 50 < 25); dsp.Process(block.data(), 512); }
    CHECK(allocation_probe::End() == 0);
}
void ZeroAllocations() {
    DspProcessor dsp; media::audio::DspControls controls;
    std::array<float, 8 * 256> pcm{};
    CHECK(dsp.Configure(192000, 8));
    allocation_probe::Begin();
    for (unsigned block = 0; block < 2000; ++block) {
        if (block % 100 == 0) {
            controls.clarity.store((block / 100) % 4); controls.spatial.store((block / 100) % 2 != 0);
            dsp.Reset();
        }
        pcm.fill(0.01f); dsp.Apply(controls); dsp.Process(pcm.data(), 256);
    }
    const auto allocations = allocation_probe::End(); CHECK(allocations == 0);
}
// Volume and mute on a near-full-scale two-tone stereo signal (0.99 peak): the taper, bit-exact unity, 20 ms ramps
// that re-aim mid-way (unmute before the mute ramp has finished), no sample ever raised (no clipping), no step
// discontinuity, exact arrival at each target, and independence from how the stream is split into blocks.
constexpr unsigned kVolumeFrames = 24000;
float volume_in[kVolumeFrames * 2], volume_a[kVolumeFrames * 2], volume_b[kVolumeFrames * 2];
void Volume() {
    using media::audio::OutputGain;
    const auto db = [](float gain) { return 20 * std::log10(static_cast<double>(gain)); };
    CHECK(OutputGain::Gain(1, false) == 1 && OutputGain::Gain(0, false) == 0 && OutputGain::Gain(1, true) == 0 &&
          OutputGain::Gain(std::numeric_limits<float>::quiet_NaN(), false) == 0 && OutputGain::Gain(2, false) == 2 &&
          OutputGain::Gain(1.5f, false) == 1.5f && OutputGain::Gain(9, false) == 2); // API 16: a boost to x2 at most.
    CHECK(std::fabs(OutputGain::Decibels(2) - 6.0206f) < 1e-3f);
    {
        // The boost's soft knee: x2 on a full-scale sine never reaches 1 and is continuous; a quiet passage is exactly doubled.
        OutputGain boost; CHECK(boost.Configure(48000, 2));
        boost.SetTarget(2.0f);
        std::vector<float> warm(2 * 4800, 0.0f); boost.Process(warm.data(), 4800); // Through the 20 ms ramp.
        std::vector<float> loud(2 * 4800), quiet(2 * 4800);
        for (unsigned f = 0; f < 4800; ++f) {
            const float v = std::sin(2 * 3.14159265f * 440 * f / 48000.0f);
            loud[2 * f] = loud[2 * f + 1] = v; quiet[2 * f] = quiet[2 * f + 1] = 0.25f * v;
        }
        const auto quiet_in = quiet;
        boost.Process(loud.data(), 4800); boost.Process(quiet.data(), 4800);
        float peak = 0, jump = 0;
        for (std::size_t i = 0; i < loud.size(); ++i) { peak = std::max(peak, std::fabs(loud[i])); if (i >= 2) jump = std::max(jump, std::fabs(loud[i] - loud[i - 2])); }
        bool doubled = true; for (std::size_t i = 0; i < quiet.size(); ++i) doubled = doubled && quiet[i] == 2.0f * quiet_in[i];
        CHECK(peak < 1.0f && peak > 0.98f && jump < 0.2f && doubled && boost.Stats().peak < 1.0f);
        std::printf("Boost x2: a full-scale sine peaks at %.4f (knee at -1 dBFS), largest step %.4f; a -12 dBFS sine exactly doubled\n", peak, jump);
    }
    CHECK(std::fabs(db(OutputGain::Gain(0.95f, false)) + 3) < 1e-3 && std::fabs(db(OutputGain::Gain(0.5f, false)) + 30) < 1e-3);
    CHECK(std::fabs(OutputGain::Decibels(0.8f) + 12) < 1e-4f && std::isinf(OutputGain::Decibels(0)));
    for (unsigned f = 0; f < kVolumeFrames; ++f) { volume_in[2 * f] = Tone(f, 1000, 0.99f); volume_in[2 * f + 1] = Tone(f, 1250, 0.99f); }
    const float half = OutputGain::Gain(0.5f, false);
    struct Change { unsigned frame; float gain; };
    constexpr unsigned ramp = rate / 50; // 20 ms.
    const Change script[]{{2000, half}, {4000, 0}, {4000 + ramp / 2, half}, {9000, 1}};
    unsigned ramps = 0; float max_step = 0;
    const auto run = [&](float* out, unsigned block) {
        std::memcpy(out, volume_in, sizeof(volume_in));
        OutputGain gain; if (!gain.Configure(rate, 2)) return false;
        unsigned f = 0, s = 0;
        while (f < kVolumeFrames) {
            while (s < 4 && script[s].frame <= f) gain.SetTarget(script[s++].gain);
            unsigned next = (std::min)(f + block, kVolumeFrames);
            if (s < 4 && script[s].frame < next) next = script[s].frame;
            gain.Process(out + 2 * f, next - f); f = next;
        }
        ramps = gain.Stats().ramps; max_step = gain.Stats().max_step; return true;
    };
    allocation_probe::Begin();
    OutputGain unity; CHECK(unity.Configure(rate, 2));
    std::memcpy(volume_a, volume_in, sizeof(volume_in)); unity.Process(volume_a, kVolumeFrames);
    CHECK(std::memcmp(volume_a, volume_in, sizeof(volume_in)) == 0); // Unity is an exact passthrough.
    CHECK(run(volume_b, 4096) && run(volume_a, 1));
    CHECK(allocation_probe::End() == 0);
    CHECK(std::memcmp(volume_a, volume_b, sizeof(volume_a)) == 0); // Block-size independent.
    CHECK(ramps == 4 && max_step <= 1.0f / ramp + 1e-7f);
    float peak = 0, raised = 0, excess = 0;
    for (unsigned i = 0; i < kVolumeFrames * 2; ++i) {
        peak = (std::max)(peak, std::fabs(volume_a[i])); raised = (std::max)(raised, std::fabs(volume_a[i]) - std::fabs(volume_in[i]));
        // A ramp adds at most one gain step per frame to the signal's own slope: no step, no click.
        if (i >= 2) excess = (std::max)(excess, std::fabs(volume_a[i] - volume_a[i - 2]) - std::fabs(volume_in[i] - volume_in[i - 2]));
    }
    // Each target is reached exactly 20 ms after it was set (the unmute re-aims the unfinished mute ramp), and held.
    const auto holds = [&](unsigned from, unsigned to, float gain) {
        for (unsigned i = 2 * from; i < 2 * to; ++i) if (volume_a[i] != volume_in[i] * gain) return false;
        return true;
    };
    const bool arrived = holds(0, 2000, 1) && holds(2000 + ramp, 4000, half) && holds(4000 + ramp / 2 + ramp, 9000, half) &&
                         std::memcmp(volume_a + 2 * (9000 + ramp), volume_in + 2 * (9000 + ramp), sizeof(float) * 2 * (kVolumeFrames - 9000 - ramp)) == 0;
    const float lowest = std::fabs(volume_a[2 * (4000 + ramp / 2) - 2] / volume_in[2 * (4000 + ramp / 2) - 2]);
    std::printf("Volume: taper -3 dB at 95 %%, -30 dB at 50 %%; 4 ramps of %u frames, largest step %.6f; peak %.4f, raised %.2g, slope excess %.6f; mute re-aimed at gain %.3f\n",
                ramp, static_cast<double>(max_step), static_cast<double>(peak), static_cast<double>(raised), static_cast<double>(excess), static_cast<double>(lowest));
    CHECK(peak <= 0.99f && raised <= 0 && excess <= 0.99f / ramp + 1e-6f && arrived && lowest > 0 && lowest < half);
}
}
int main() {
    ConfigureAndBypass(); Dialogue(); ClippingAndCompression(); Spatial(); FoldDown(); ResetAndBlockInvariance(); ZeroAllocations(); Volume();
    std::printf("Audio DSP: %d failures\n", failures);
    return failures ? 1 : 0;
}
