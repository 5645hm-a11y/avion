// Offline tool (not a test), 2026-11-05: the platform's AV1 decoders (Media Foundation transforms) on this machine, and how fast
// the software one decodes a file's AV1 track into system-memory NV12.
//   media_av1_mft_probe <file.mp4|.mkv> [pictures]
#include "media/demux/Demuxer.hpp"
#include "media/platform/FileBlockSource.hpp"
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s file [pictures]\n", argv[0]); return 2; }
    const unsigned wanted = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 300;
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) return 1;
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_AV1};
    IMFActivate** list = nullptr; UINT32 count = 0;
    const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_FIELDOFUSE | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &input, nullptr, &list, &count);
    std::printf("MFTEnumEx: 0x%08lx, %u AV1 decoder(s)\n", static_cast<unsigned long>(hr), count);
    int chosen = -1;
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t name[256]{}; UINT32 length = 0; UINT32 mft_flags = 0;
        (void)list[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, &length);
        (void)list[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &mft_flags);
        std::printf("  %u: %ls flags 0x%x%s%s\n", i, name, mft_flags, (mft_flags & MFT_ENUM_FLAG_HARDWARE) ? " hardware" : "", (mft_flags & MFT_ENUM_FLAG_ASYNCMFT) ? " async" : "");
        if (chosen < 0 && !(mft_flags & MFT_ENUM_FLAG_HARDWARE)) chosen = static_cast<int>(i);
    }
    if (chosen < 0) { std::puts("no software AV1 decoder"); return 1; }
    IMFTransform* mft = nullptr;
    hr = list[chosen]->ActivateObject(IID_PPV_ARGS(&mft));
    std::printf("ActivateObject: 0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 1;
    IMFAttributes* attributes = nullptr;
    if (SUCCEEDED(mft->GetAttributes(&attributes))) {
        UINT32 async = 0; (void)attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        std::printf("attributes: async %u\n", async);
        if (async) (void)attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attributes->Release();
    }
    // The file's AV1 track.
    media::core::LinearMemoryArena arena; if (!arena.Initialize(160u << 20)) return 1;
    media::platform::FileBlockSource source; if (!source.Open(arena, argv[1], 8u << 20)) { std::puts("cannot open"); return 1; }
    media::demux::Demuxer d; if (!d.Initialize(arena, source, {16, 48u << 20, 1})) return 1;
    media::core::SpscRingBuffer<media::core::MediaPacket, 8> q;
    while (!d.Seekable()) { const auto s = d.Step(q); if (s == media::demux::ParseStatus::Failed) { std::puts("parse failed"); return 1; } while (const auto* p = q.PeekReadSlot()) { (void)d.ReturnPacket(*p); (void)q.ReleaseReadSlot(); } }
    int track = -1;
    for (unsigned i = 0; i < d.TrackCount(); ++i) if (d.Track(i)->codec == media::demux::Codec::Av1) track = static_cast<int>(i);
    if (track < 0) { std::puts("no AV1 track"); return 1; }
    const auto& info = *d.Track(static_cast<unsigned>(track));
    std::printf("track %d: %ux%u, %u samples, av1C %llu bytes\n", track, info.width, info.height, info.sample_count, static_cast<unsigned long long>(info.configuration.bytes));
    IMFMediaType* type = nullptr; (void)MFCreateMediaType(&type);
    (void)type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); (void)type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_AV1);
    (void)MFSetAttributeSize(type, MF_MT_FRAME_SIZE, info.width, info.height);
    hr = mft->SetInputType(0, type, 0);
    std::printf("SetInputType: 0x%08lx\n", static_cast<unsigned long>(hr));
    IMFMediaType* out = nullptr;
    for (DWORD i = 0; i < 32; ++i) {
        IMFMediaType* available = nullptr;
        if (FAILED(mft->GetOutputAvailableType(0, i, &available))) break;
        GUID subtype{}; (void)available->GetGUID(MF_MT_SUBTYPE, &subtype);
        std::printf("  output %lu: %08lx\n", i, subtype.Data1);
        if (!out && (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_P010)) out = available; else available->Release();
    }
    hr = out ? mft->SetOutputType(0, out, 0) : E_FAIL;
    std::printf("SetOutputType NV12: 0x%08lx\n", static_cast<unsigned long>(hr));
    MFT_OUTPUT_STREAM_INFO stream{}; (void)mft->GetOutputStreamInfo(0, &stream);
    std::printf("output stream: flags 0x%lx, %lu bytes%s\n", stream.dwFlags, stream.cbSize, (stream.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) ? " (provides samples)" : "");
    (void)mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0); (void)mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    IMFSample* own = nullptr; IMFMediaBuffer* own_buffer = nullptr;
    if (!(stream.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) { (void)MFCreateSample(&own); (void)MFCreateMemoryBuffer(stream.cbSize ? stream.cbSize : info.width * info.height * 2, &own_buffer); (void)own->AddBuffer(own_buffer); }
    std::vector<unsigned char> bytes(64u << 20);
    unsigned fed = 0, decoded = 0; double busy = 0; LONGLONG first_time = -1, last_time = 0;
    FILE* f = nullptr; if (fopen_s(&f, argv[1], "rb") != 0) return 1;
    const auto began = std::chrono::steady_clock::now();
    for (std::uint32_t s = 0; s < info.sample_count && decoded < wanted; ++s) {
        media::demux::SampleRecord r{};
        if (!d.SampleAt(static_cast<unsigned>(track), s, r) || r.bytes > bytes.size()) break;
        _fseeki64(f, static_cast<long long>(r.offset), SEEK_SET); (void)fread(bytes.data(), 1, r.bytes, f);
        IMFSample* sample = nullptr; IMFMediaBuffer* buffer = nullptr;
        (void)MFCreateSample(&sample); (void)MFCreateMemoryBuffer(r.bytes, &buffer);
        BYTE* p = nullptr; (void)buffer->Lock(&p, nullptr, nullptr); std::memcpy(p, bytes.data(), r.bytes); (void)buffer->Unlock(); (void)buffer->SetCurrentLength(r.bytes);
        (void)sample->AddBuffer(buffer); (void)sample->SetSampleTime(r.pts_100ns); (void)sample->SetSampleDuration(r.duration_100ns);
        const auto t0 = std::chrono::steady_clock::now();
        hr = mft->ProcessInput(0, sample, 0);
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER o{0, own, 0, nullptr}; DWORD status = 0;
            if (own_buffer) (void)own_buffer->SetCurrentLength(0);
            const HRESULT h = mft->ProcessOutput(0, 1, &o, &status);
            if (o.pEvents) o.pEvents->Release();
            if (h == MF_E_TRANSFORM_STREAM_CHANGE) {
                IMFMediaType* again = nullptr; (void)mft->GetOutputAvailableType(0, 0, &again); (void)mft->SetOutputType(0, again, 0); again->Release();
                (void)mft->GetOutputStreamInfo(0, &stream);
                std::printf("stream change: output %lu bytes\n", stream.cbSize);
                if (own_buffer && stream.cbSize) { own_buffer->Release(); (void)MFCreateMemoryBuffer(stream.cbSize, &own_buffer); (void)own->RemoveAllBuffers(); (void)own->AddBuffer(own_buffer); }
                continue;
            }
            if (FAILED(h)) break;
            LONGLONG t = 0; (void)o.pSample->GetSampleTime(&t);
            if (first_time < 0) first_time = t;
            last_time = t; ++decoded;
            if (!own) o.pSample->Release();
        }
        busy += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) { std::printf("ProcessInput 0x%08lx at %u\n", static_cast<unsigned long>(hr), s); break; }
        ++fed; sample->Release(); buffer->Release();
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::printf("fed %u, decoded %u pictures in %.2f s wall (%.1f per second), decode calls %.2f ms per picture; times %.3f .. %.3f s\n", fed, decoded, wall,
                decoded / wall, decoded ? busy / decoded : 0.0, first_time / 1e7, last_time / 1e7);
    fclose(f);
    return 0;
}
