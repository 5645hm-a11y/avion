// Seek pre-roll for AAC-LC through the production Win32AacDecoder (Microsoft AAC MFT). A continuous decode
// is the reference: after a flush, decoding restarted at the target access unit lacks the previous frame's
// MDCT overlap, while a pre-rolled restart (earlier access units decoded and discarded) must reproduce the
// reference exactly from the target sample on, and the armed fade-in must be an exact linear ramp over it.
#include "media/platform/windows/Win32AacDecoder.hpp"
#include "allocation_probe.hpp"
#include <mferror.h>
#include <wmcodecdsp.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace media::platform::windows;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr unsigned kRate = 48000, kChannels = 2, kPackets = 48, kFrame = 1024;
constexpr std::int64_t kDuration = 1024ll * 10000000 / kRate;
bool Ok(HRESULT hr) { if (FAILED(hr)) { std::printf("native failure %08lx\n", static_cast<unsigned long>(hr)); return false; } return true; }
// Test-setup encoder (allocates; runs before any measurement): two tones, as in the playback fixture.
bool Encode(std::vector<std::vector<std::byte>>& packets) {
    IMFTransform* encoder = nullptr;
    if (!Ok(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder)))) return false;
    bool good = true;
    for (unsigned output = 0; output != 2 && good; ++output) {
        IMFMediaType* type = nullptr; good = Ok(MFCreateMediaType(&type));
        if (good) good = Ok(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) && Ok(type->SetGUID(MF_MT_SUBTYPE, output ? MFAudioFormat_AAC : MFAudioFormat_PCM)) &&
            Ok(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate)) && Ok(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels)) &&
            Ok(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        if (good && output) good = Ok(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000)) && Ok(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0)) &&
            Ok(encoder->SetOutputType(0, type, 0));
        else if (good) good = Ok(encoder->SetInputType(0, type, 0));
        if (type) type->Release();
    }
    good = good && Ok(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) && Ok(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    auto drain = [&] {
        for (;;) {
            IMFSample* sample = nullptr; IMFMediaBuffer* buffer = nullptr;
            if (!Ok(MFCreateSample(&sample)) || !Ok(MFCreateMemoryBuffer(16384, &buffer)) || !Ok(sample->AddBuffer(buffer))) return false;
            MFT_OUTPUT_DATA_BUFFER result{0, sample, 0, nullptr}; DWORD flags = 0;
            const auto hr = encoder->ProcessOutput(0, 1, &result, &flags);
            if (result.pEvents) result.pEvents->Release();
            bool more = false;
            if (SUCCEEDED(hr)) {
                BYTE* data = nullptr; DWORD length = 0;
                if (Ok(buffer->Lock(&data, nullptr, &length))) {
                    packets.emplace_back(reinterpret_cast<const std::byte*>(data), reinterpret_cast<const std::byte*>(data) + length);
                    (void)buffer->Unlock(); more = true;
                }
            }
            buffer->Release(); sample->Release();
            if (!more) return hr == MF_E_TRANSFORM_NEED_MORE_INPUT;
        }
    };
    for (unsigned frame = 0; frame < kPackets + 2 && good; ++frame) {
        IMFSample* sample = nullptr; IMFMediaBuffer* buffer = nullptr;
        good = Ok(MFCreateSample(&sample)) && Ok(MFCreateMemoryBuffer(kFrame * 4, &buffer)) && Ok(sample->AddBuffer(buffer));
        BYTE* bytes = nullptr;
        if (good && Ok(buffer->Lock(&bytes, nullptr, nullptr))) {
            auto* pcm = reinterpret_cast<std::int16_t*>(bytes);
            for (unsigned i = 0; i < kFrame; ++i) {
                const double t = static_cast<double>(frame * kFrame + i) / kRate;
                pcm[i * 2] = static_cast<std::int16_t>(6000 * std::sin(t * 440 * 6.283185307179586));
                pcm[i * 2 + 1] = static_cast<std::int16_t>(6000 * std::sin(t * 1000 * 6.283185307179586));
            }
            (void)buffer->Unlock();
        }
        good = good && Ok(buffer->SetCurrentLength(kFrame * 4)) && Ok(sample->SetSampleTime(static_cast<LONGLONG>(frame) * kDuration)) &&
               Ok(sample->SetSampleDuration(kDuration)) && Ok(encoder->ProcessInput(0, sample, 0)) && drain();
        if (buffer) buffer->Release();
        if (sample) sample->Release();
    }
    encoder->Release();
    return good && packets.size() >= kPackets;
}
}
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 1;
    std::vector<std::vector<std::byte>> packets;
    if (!Encode(packets)) { std::puts("SKIP no AAC encoder"); return 77; }
    const std::byte asc[2]{std::byte{0x11}, std::byte{0x90}}; // AAC-LC, 48 kHz, stereo.
    Win32AacDecoder decoder;
    if (!decoder.Prepare(asc, kRate, kChannels)) { std::printf("decoder preparation failed %08lx\n", static_cast<unsigned long>(decoder.LastError())); return 1; }
    constexpr unsigned kSamples = (kPackets + 1) * kFrame * kChannels;
    std::vector<float> reference(kSamples), output(kSamples);
    auto bytes = [](std::vector<float>& v, std::size_t frame) { return std::as_writable_bytes(std::span(v)).subspan(frame * kChannels * 4); };
    auto decode = [&](unsigned packet) { return decoder.Decode(packets[packet], static_cast<std::int64_t>(packet) * kDuration, kDuration); };
    // Continuous reference. The MFT withholds its first output, so PCM is timed by counting output frames
    // from the first decoded access unit, exactly as the engine clocks it; the drain yields the last unit.
    unsigned total = 0, withheld = 0;
    for (unsigned p = 0; p < kPackets; ++p) {
        CHECK(decode(p)); if (!decoder.Pending()) ++withheld;
        total += decoder.Read(bytes(reference, total));
    }
    CHECK(decoder.Drain()); total += decoder.Read(bytes(reference, total));
    CHECK(total == kPackets * kFrame && withheld == 1);
    // Restart as the engine's seek does: decode from `preroll` units before the target's unit, drop output
    // frames before the target frame by count (through the drain when needed), arm the fade on the first kept
    // frame. Returns the frames kept in `output`, which must continue the reference from `target`.
    auto restart = [&](unsigned target, unsigned preroll, unsigned fade, unsigned frames) {
        const unsigned unit = target / kFrame, first = unit - (std::min)(unit, preroll);
        unsigned produced = 0, kept = 0; bool armed = false;
        CHECK(decoder.Flush());
        auto take = [&] {
            const auto pending = decoder.PendingFrames();
            const auto skip = produced + pending <= target - first * kFrame ? pending : (std::max)(target - first * kFrame, produced) - produced;
            CHECK(decoder.Skip(skip) == skip); produced += pending;
            if (decoder.Pending() && !armed) { decoder.FadeIn(fade); armed = true; }
            kept += decoder.Read(bytes(output, kept));
        };
        for (unsigned p = first; p < kPackets && kept < frames; ++p) { CHECK(decode(p)); take(); }
        if (kept < frames) { CHECK(decoder.Drain()); take(); }
        return kept;
    };
    auto error = [&](unsigned target, unsigned frames, unsigned from = 0) {
        double worst = 0;
        for (unsigned i = from * kChannels; i < frames * kChannels; ++i)
            worst = (std::max)(worst, std::abs(static_cast<double>(output[i]) - reference[target * kChannels + i]));
        return worst;
    };
    constexpr unsigned kTarget = 20 * kFrame + 300, kSpan = 2 * kFrame; // 300 frames into access unit 20.
    constexpr unsigned kLast = (kPackets - 1) * kFrame + 700;           // Inside the final unit (drain path).
    allocation_probe::Begin();
    CHECK(restart(kTarget, 0, 0, kSpan) >= kSpan); const double cold = error(kTarget, kSpan);
    CHECK(restart(kTarget, 1, 0, kSpan) >= kSpan); const double one = error(kTarget, kSpan);
    CHECK(restart(kTarget, 2, 0, kSpan) >= kSpan); const double two = error(kTarget, kSpan);
    CHECK(restart(kLast, 2, 0, kFrame - 700) == kFrame - 700); const double last = error(kLast, kFrame - 700);
    // Fade-in: an exact linear ramp over 120 frames (2.5 ms), then the untouched reference.
    CHECK(restart(kTarget, 2, 120, kSpan) >= kSpan);
    double ramp = 0;
    for (unsigned i = 0; i < 120; ++i)
        for (unsigned c = 0; c < kChannels; ++c)
            ramp = (std::max)(ramp, std::abs(static_cast<double>(output[i * kChannels + c]) -
                                             static_cast<double>(reference[(kTarget + i) * kChannels + c] * (static_cast<float>(i) / 120.0f))));
    const double after_ramp = error(kTarget, kSpan, 120);
    const auto allocations = allocation_probe::End();
    double peak = 0; for (float v : reference) peak = (std::max)(peak, static_cast<double>(std::abs(v)));
    std::printf("AAC restart error vs continuous decode (peak %.4f): no pre-roll %.4f, pre-roll 1 unit %.2e, 2 units %.2e, "
                "final unit via drain %.2e; fade ramp %.2e, after ramp %.2e; allocations %llu\n",
                peak, cold, one, two, last, ramp, after_ramp, static_cast<unsigned long long>(allocations));
    CHECK(peak > 0.1);
    CHECK(cold > peak * 0.01);                     // Without pre-roll the restart is audibly wrong...
    CHECK(one < 1e-6 && two < 1e-6 && last < 1e-6); // ...with it, the target frame onward is the continuous decode.
    CHECK(ramp < 1e-6 && after_ramp < 1e-6);
    CHECK(allocations == 0);
    decoder.Close(); MFShutdown(); CoUninitialize();
    std::printf("AAC pre-roll: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}