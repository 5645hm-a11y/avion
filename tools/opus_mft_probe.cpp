// Offline tool (not a test), 2026-11-05: the platform's Opus decoder (a Media Foundation transform) on a file's Opus track: what
// configuration it takes, and the PCM it gives per packet.
//   media_opus_mft_probe <file.webm|.mkv|.mp4> [packets]
#include "media/demux/Demuxer.hpp"
#include "media/platform/FileBlockSource.hpp"
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <cstdio>
#include <cstring>
#include <vector>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const unsigned wanted = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 20;
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) return 1;
    media::core::LinearMemoryArena arena; if (!arena.Initialize(80u << 20)) return 1;
    media::platform::FileBlockSource source; if (!source.Open(arena, argv[1], 1u << 20)) return 1;
    media::demux::Demuxer d; if (!d.Initialize(arena, source, {16, 16u << 20, 1})) return 1;
    media::core::SpscRingBuffer<media::core::MediaPacket, 8> q;
    while (!d.Seekable()) { if (d.Step(q) == media::demux::ParseStatus::Failed) return 1; while (const auto* p = q.PeekReadSlot()) { (void)d.ReturnPacket(*p); (void)q.ReleaseReadSlot(); } }
    int track = -1;
    for (unsigned i = 0; i < d.TrackCount(); ++i) if (d.Track(i)->codec == media::demux::Codec::Opus) track = static_cast<int>(i);
    if (track < 0) { std::puts("no Opus track"); return 1; }
    const auto& info = *d.Track(static_cast<unsigned>(track));
    FILE* f = nullptr; if (fopen_s(&f, argv[1], "rb") != 0) return 1;
    std::vector<unsigned char> head(static_cast<std::size_t>(info.configuration.bytes));
    _fseeki64(f, static_cast<long long>(info.configuration.offset), SEEK_SET); (void)fread(head.data(), 1, head.size(), f);
    std::printf("Opus: %u ch, %u Hz, configuration %zu bytes, %u packets\n", info.channels, info.sample_rate, head.size(), info.sample_count);
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Audio, MFAudioFormat_Opus};
    IMFActivate** list = nullptr; UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &input, nullptr, &list, &count);
    std::printf("MFTEnumEx 0x%08lx: %u\n", static_cast<unsigned long>(hr), count);
    if (!count) return 1;
    IMFTransform* mft = nullptr; hr = list[0]->ActivateObject(IID_PPV_ARGS(&mft));
    for (int variant = 0; variant < 3; ++variant) {
        IMFMediaType* type = nullptr; (void)MFCreateMediaType(&type);
        (void)type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); (void)type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Opus);
        (void)type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000); (void)type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, info.channels);
        if (variant == 1) (void)type->SetBlob(MF_MT_USER_DATA, head.data(), static_cast<UINT32>(head.size()));
        if (variant == 2 && head.size() > 8) (void)type->SetBlob(MF_MT_USER_DATA, head.data() + 8, static_cast<UINT32>(head.size() - 8));
        hr = mft->SetInputType(0, type, 0);
        std::printf("SetInputType variant %d: 0x%08lx\n", variant, static_cast<unsigned long>(hr));
        type->Release();
        if (SUCCEEDED(hr)) break;
    }
    IMFMediaType* out = nullptr;
    for (DWORD i = 0; i < 16; ++i) {
        IMFMediaType* a = nullptr; if (FAILED(mft->GetOutputAvailableType(0, i, &a))) break;
        GUID sub{}; UINT32 rate = 0, ch = 0, bits = 0; (void)a->GetGUID(MF_MT_SUBTYPE, &sub); (void)a->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        (void)a->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch); (void)a->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        std::printf("  output %lu: %s %u Hz %u ch %u bits\n", i, sub == MFAudioFormat_Float ? "float" : sub == MFAudioFormat_PCM ? "pcm" : "?", rate, ch, bits);
        if (!out && sub == MFAudioFormat_Float) out = a; else a->Release();
    }
    hr = out ? mft->SetOutputType(0, out, 0) : E_FAIL;
    std::printf("SetOutputType float: 0x%08lx\n", static_cast<unsigned long>(hr));
    MFT_OUTPUT_STREAM_INFO so{}; (void)mft->GetOutputStreamInfo(0, &so);
    std::printf("output stream flags 0x%lx, %lu bytes\n", so.dwFlags, so.cbSize);
    (void)mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); (void)mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    IMFSample* own = nullptr; IMFMediaBuffer* own_buffer = nullptr;
    (void)MFCreateSample(&own); (void)MFCreateMemoryBuffer(1u << 20, &own_buffer); (void)own->AddBuffer(own_buffer);
    std::vector<unsigned char> bytes(1u << 20);
    unsigned total = 0;
    for (std::uint32_t s = 0; s < info.sample_count && s < wanted; ++s) {
        media::demux::SampleRecord r{}; if (!d.SampleAt(static_cast<unsigned>(track), s, r)) break;
        _fseeki64(f, static_cast<long long>(r.offset), SEEK_SET); (void)fread(bytes.data(), 1, r.bytes, f);
        IMFSample* sample = nullptr; IMFMediaBuffer* buffer = nullptr; (void)MFCreateSample(&sample); (void)MFCreateMemoryBuffer(r.bytes, &buffer);
        BYTE* p = nullptr; (void)buffer->Lock(&p, nullptr, nullptr); std::memcpy(p, bytes.data(), r.bytes); (void)buffer->Unlock(); (void)buffer->SetCurrentLength(r.bytes);
        (void)sample->AddBuffer(buffer); (void)sample->SetSampleTime(r.pts_100ns); (void)sample->SetSampleDuration(r.duration_100ns);
        hr = mft->ProcessInput(0, sample, 0);
        unsigned produced = 0; LONGLONG t = -1;
        for (;;) {
            (void)own_buffer->SetCurrentLength(0);
            MFT_OUTPUT_DATA_BUFFER o{0, own, 0, nullptr}; DWORD status = 0;
            const HRESULT h = mft->ProcessOutput(0, 1, &o, &status);
            if (o.pEvents) o.pEvents->Release();
            if (FAILED(h)) break;
            DWORD n = 0; (void)own_buffer->GetCurrentLength(&n); produced += n / (4u * info.channels); (void)own->GetSampleTime(&t);
        }
        total += produced;
        std::printf("packet %u: %u bytes pts %.4f dur %.4f in 0x%08lx -> %u frames (out time %.4f)\n", s, r.bytes, r.pts_100ns / 1e7, r.duration_100ns / 1e7,
                    static_cast<unsigned long>(hr), produced, t / 1e7);
        sample->Release(); buffer->Release();
    }
    std::printf("total %u frames\n", total);
    return 0;
}
