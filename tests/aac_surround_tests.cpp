// Multichannel AAC through the production Win32AacDecoder (Microsoft AAC MFT): channel negotiation, the decoder's
// WAVEFORMATEXTENSIBLE mask, per-speaker routing (a distinct tone per input speaker), PCE-described layouts, layout
// mapping onto 5.1, and the platform's 7.1 refusal. 5.1 is encoded natively during setup. The platform encoder makes
// only 1, 2 or 6 channels, and its frames cannot be split into elements without a Huffman-level parser, so other
// layouts use constructed silent elements (ONLY_LONG, max_sfb 0: valid AAC-LC that the MFT decodes frame for frame).
#include "media/platform/windows/Win32AacDecoder.hpp"
#include "allocation_probe.hpp"
#include <mferror.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace media::platform::windows;
namespace speaker = media::codec::speaker;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
using Bytes = std::vector<std::byte>;
constexpr unsigned kRate = 48000, kPackets = 40;
constexpr std::int64_t kDuration = 1024ll * 10000000 / kRate;
constexpr double kHz[8]{300, 500, 700, 60, 1100, 1300, 1700, 1900}; // FL FR FC LFE BL BR SL SR.
bool Ok(HRESULT hr) { return SUCCEEDED(hr); }
// Native encode of `channels` (1, 2 or 6) with speaker tones kHz[first + c]. Setup only (allocates).
std::vector<Bytes> Encode(unsigned channels, unsigned first, Bytes* asc = nullptr) {
    std::vector<Bytes> packets;
    IMFTransform* encoder = nullptr;
    if (!Ok(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder)))) return packets;
    bool good = true;
    for (unsigned output = 0; output != 2 && good; ++output) {
        IMFMediaType* type = nullptr; good = Ok(MFCreateMediaType(&type));
        good = good && Ok(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) && Ok(type->SetGUID(MF_MT_SUBTYPE, output ? MFAudioFormat_AAC : MFAudioFormat_PCM)) &&
               Ok(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate)) && Ok(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels)) &&
               Ok(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        if (good && channels == 6) good = Ok(type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, speaker::Surround51));
        if (good && output) good = Ok(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, channels == 6 ? 24000 : 16000)) &&
                                   Ok(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0)) && Ok(encoder->SetOutputType(0, type, 0));
        else if (good) good = Ok(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2)) &&
                              Ok(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, channels * 2 * kRate)) && Ok(encoder->SetInputType(0, type, 0));
        if (type) type->Release();
    }
    if (good && asc) {
        IMFMediaType* current = nullptr; UINT32 size = 0; std::uint8_t user[64]{};
        good = Ok(encoder->GetOutputCurrentType(0, &current)) && Ok(current->GetBlob(MF_MT_USER_DATA, user, sizeof(user), &size)) && size > 12;
        if (current) current->Release();
        if (good) asc->assign(reinterpret_cast<const std::byte*>(user) + 12, reinterpret_cast<const std::byte*>(user) + size);
    }
    good = good && Ok(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) && Ok(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    for (unsigned frame = 0; frame < kPackets + 2 && good; ++frame) {
        IMFSample* sample = nullptr; IMFMediaBuffer* buffer = nullptr; BYTE* bytes = nullptr;
        good = Ok(MFCreateSample(&sample)) && Ok(MFCreateMemoryBuffer(1024 * channels * 2, &buffer)) && Ok(sample->AddBuffer(buffer)) &&
               Ok(buffer->Lock(&bytes, nullptr, nullptr));
        if (good) {
            auto* pcm = reinterpret_cast<std::int16_t*>(bytes);
            for (unsigned i = 0; i < 1024; ++i) for (unsigned c = 0; c < channels; ++c)
                pcm[i * channels + c] = static_cast<std::int16_t>(4000 * std::sin((frame * 1024.0 + i) / kRate * kHz[first + c] * 6.283185307179586));
            (void)buffer->Unlock();
            good = Ok(buffer->SetCurrentLength(1024 * channels * 2)) && Ok(sample->SetSampleTime(frame * kDuration)) &&
                   Ok(sample->SetSampleDuration(kDuration)) && Ok(encoder->ProcessInput(0, sample, 0));
        }
        if (buffer) buffer->Release();
        if (sample) sample->Release();
        for (bool more = good; more;) {
            IMFSample* out = nullptr; IMFMediaBuffer* data = nullptr; more = false;
            if (!Ok(MFCreateSample(&out)) || !Ok(MFCreateMemoryBuffer(65536, &data)) || !Ok(out->AddBuffer(data))) { good = false; }
            else {
                MFT_OUTPUT_DATA_BUFFER result{0, out, 0, nullptr}; DWORD flags = 0;
                const auto hr = encoder->ProcessOutput(0, 1, &result, &flags);
                if (result.pEvents) result.pEvents->Release();
                BYTE* p = nullptr; DWORD length = 0;
                if (SUCCEEDED(hr) && Ok(data->Lock(&p, nullptr, &length))) {
                    packets.emplace_back(reinterpret_cast<const std::byte*>(p), reinterpret_cast<const std::byte*>(p) + length); (void)data->Unlock(); more = true;
                } else if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) good = false;
            }
            if (data) data->Release();
            if (out) out->Release();
        }
    }
    encoder->Release();
    if (!good) packets.clear();
    return packets;
}
struct Bits {
    Bytes b; unsigned n = 0;
    void Put(unsigned v, unsigned count) { while (count) { if (!(n % 8)) b.push_back(std::byte{0}); if ((v >> --count) & 1) b.back() |= std::byte{static_cast<unsigned char>(0x80 >> (n % 8))}; ++n; } }
    void Align() { while (n % 8) Put(0, 1); }
    void SilentIcs() { Put(100, 8); Put(0, 1); Put(0, 2); Put(0, 1); Put(0, 6); Put(0, 1); Put(0, 1); Put(0, 1); Put(0, 1); }
};
// One silent raw_data_block: elements as (id, tag): 0 SCE, 1 CPE (common_window 0), 3 LFE; then ID_END.
Bytes Silent(std::initializer_list<std::pair<unsigned, unsigned>> elements) {
    Bits b;
    for (const auto& [id, tag] : elements) { b.Put(id, 3); b.Put(tag, 4); if (id == 1) { b.Put(0, 1); b.SilentIcs(); b.SilentIcs(); } else b.SilentIcs(); }
    b.Put(7, 3); b.Align(); return b.b;
}
Bytes Asc(unsigned configuration) { Bits b; b.Put(2, 5); b.Put(3, 4); b.Put(configuration, 4); b.Put(0, 3); return b.b; }
// PCE: front SCE 0 + CPE 0, optional side CPE, back CPE, LFE 0 (tags given: the MFT matches elements by tag).
Bytes Pce(bool side, unsigned back_tag) {
    Bits b; b.Put(2, 5); b.Put(3, 4); b.Put(0, 4); b.Put(0, 3);
    b.Put(0, 4); b.Put(1, 2); b.Put(3, 4); b.Put(2, 4); b.Put(side ? 1 : 0, 4); b.Put(1, 4); b.Put(1, 2); b.Put(0, 3); b.Put(0, 4); b.Put(0, 3);
    b.Put(0, 1); b.Put(0, 4); b.Put(1, 1); b.Put(0, 4); if (side) { b.Put(1, 1); b.Put(1, 4); } b.Put(1, 1); b.Put(back_tag, 4); b.Put(0, 4);
    b.Align(); b.Put(0, 8); return b.b;
}
float pcm[64 * 1024 * 8]; // Decoded output of one run (fixed storage).
// Decodes every packet, returns frames written to `pcm` (interleaved in the decoder's output layout).
unsigned DecodeAll(Win32AacDecoder& decoder, const std::vector<Bytes>& packets) {
    unsigned frames = 0; const unsigned channels = decoder.Channels();
    for (std::size_t i = 0; i < packets.size(); ++i) {
        if (!decoder.Decode(packets[i], static_cast<std::int64_t>(i) * kDuration, kDuration)) return frames;
        while (decoder.Pending() && frames < 64 * 1024)
            frames += decoder.Read({reinterpret_cast<std::byte*>(pcm + static_cast<std::size_t>(frames) * channels), (64 * 1024 - frames) * channels * sizeof(float)});
    }
    return frames;
}
double Amplitude(unsigned frames, unsigned channels, unsigned channel, double hz) {
    double re = 0, im = 0;
    for (unsigned i = 2048; i < frames; ++i) { const double w = i * hz * 6.283185307179586 / kRate; re += pcm[i * channels + channel] * std::cos(w); im += pcm[i * channels + channel] * std::sin(w); }
    return 2 * std::sqrt(re * re + im * im) / (frames - 2048);
}
// Each output channel's strongest speaker tone must be its own (`expected[c]`, -1 = silent channel).
void Routing(const char* name, unsigned frames, unsigned channels, const int* expected, double minimum) {
    for (unsigned c = 0; c < channels; ++c) {
        int best = -1; double best_amplitude = 0, total = 0;
        for (unsigned k = 0; k < 8; ++k) { const double a = Amplitude(frames, channels, c, kHz[k]); total += a; if (a > best_amplitude) { best_amplitude = a; best = static_cast<int>(k); } }
        std::printf("  %s ch%u: strongest %4.0f Hz, amplitude %.4f\n", name, c, best >= 0 ? kHz[best] : 0.0, best_amplitude);
        if (expected[c] < 0) CHECK(total < 1e-4);
        else CHECK(best == expected[c] && best_amplitude > minimum && best_amplitude > 0.6 * total);
    }
}
}
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 1;
    Bytes asc51;
    const auto native = Encode(6, 0, &asc51);
    if (native.size() < kPackets) { std::puts("SKIP no multichannel AAC encoder"); return 77; }
    // 1. Native 5.1 (channel configuration 6): 6 channels, mask 0x3F, identity routing in WAVEFORMATEXTENSIBLE order.
    {
        Win32AacDecoder decoder;
        CHECK(asc51.size() == 2 && std::to_integer<unsigned>(asc51[1]) == 0xb0);
        CHECK(decoder.Prepare(asc51, kRate, 6));
        CHECK(decoder.Channels() == 6 && decoder.DecoderMask() == speaker::Surround51 && decoder.OutputMask() == speaker::Surround51);
        // Steady-state decoding allocates nothing (after the first access units warm the MFT).
        for (unsigned i = 0; i < 4; ++i) { CHECK(decoder.Decode(native[i], i * kDuration, kDuration)); while (decoder.Pending()) (void)decoder.Read({reinterpret_cast<std::byte*>(pcm), sizeof(pcm)}); }
        allocation_probe::Begin();
        for (unsigned i = 4; i < 20; ++i) { CHECK(decoder.Decode(native[i], i * kDuration, kDuration)); while (decoder.Pending()) (void)decoder.Read({reinterpret_cast<std::byte*>(pcm), sizeof(pcm)}); }
        CHECK(allocation_probe::End() == 0);
        CHECK(decoder.Flush());
        const unsigned frames = DecodeAll(decoder, native); CHECK(frames >= 30 * 1024);
        const int expected[6]{0, 1, 2, 3, 4, 5};
        Routing("native 5.1", frames, 6, expected, 0.08); // Encoded at 4000/32768 = 0.122.
    }
    // 2. The same native 5.1 access units described by a PCE (front SCE 0 + CPE 0, back CPE 1, LFE 0: the encoder's element
    //    tags). The whole ASC reaches the MFT, which decodes by tag: identical routing and level.
    {
        Win32AacDecoder decoder; const auto pce = Pce(false, 1);
        CHECK(decoder.Prepare(pce, kRate, 6) && decoder.Channels() == 6 && decoder.Config().program_config && decoder.DecoderMask() == speaker::Surround51);
        const unsigned frames = DecodeAll(decoder, native); CHECK(frames >= 30 * 1024);
        const int expected[6]{0, 1, 2, 3, 4, 5};
        Routing("PCE 5.1", frames, 6, expected, 0.08);
    }
    // 3. 5.0 (channel configuration 5): the MFT reports FL FR FC BL BR (0x37); output is 5.1 through the mapping path.
    {
        Win32AacDecoder decoder;
        CHECK(decoder.Prepare(Asc(5), kRate, 5));
        CHECK(decoder.DecoderMask() == 0x37u && decoder.Channels() == 6 && decoder.OutputMask() == speaker::Surround51);
        const std::vector<Bytes> silent(20, Silent({{0, 0}, {1, 0}, {1, 1}}));
        const unsigned frames = DecodeAll(decoder, silent);
        CHECK(frames >= 18 * 1024); // Every frame decodes (the MFT withholds its first output).
        float peak = 0; for (unsigned i = 0; i < frames * 6; ++i) peak = (std::max)(peak, std::fabs(pcm[i]));
        CHECK(peak == 0);
    }
    // 4. 7.1 (configuration 12, and a PCE) and HE-AAC: parsed, then refused with kUnsupportedLayout, not a crash.
    for (const auto& asc : {Asc(12), Pce(true, 2), Bytes{std::byte{0x2b}, std::byte{0x39}, std::byte{0x88}, std::byte{0x00}}}) {
        Win32AacDecoder decoder;
        CHECK(!decoder.Prepare(asc, kRate, 8) && decoder.LastError() == Win32AacDecoder::kUnsupportedLayout && decoder.Channels() == 0);
    }
    std::printf("AAC surround: %d failures\n", failures);
    MFShutdown(); CoUninitialize();
    return failures ? 1 : 0;
}
