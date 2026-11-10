#pragma once
// Test tooling only: authors HEVC or H.264 elementary streams through the platform's own encoder MFTs
// (no bundled media, no third-party tools). Allocates freely; call it before any measured interval.
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace encoder {
template <class T> void ReleaseCom(T*& p) { if (p) { p->Release(); p = nullptr; } }
// One access unit in decode order, 4-byte length-prefixed NAL units. pts is the MFT's sample time,
// which encoders label in decode order; derive presentation order from the bitstream (POC).
struct Encoded { LONGLONG pts = 0; std::vector<std::byte> au; };
template <class F> void ForEachNal(const std::vector<std::byte>& au, F f) {
    std::size_t offset = 0;
    while (au.size() - offset >= 4) {
        std::uint32_t size = 0; for (unsigned i = 0; i < 4; ++i) size = (size << 8) | std::to_integer<unsigned>(au[offset++]);
        if (size > au.size() - offset) return;
        f(std::span<const std::byte>{au.data() + offset, size}); offset += size;
    }
}
inline void AppendAnnexB(const BYTE* data, std::size_t size, std::vector<std::byte>& out) {
    auto emit = [&](std::size_t begin, std::size_t end) {
        while (end > begin && data[end - 1] == 0) --end; // trailing_zero_8bits belong to the byte stream.
        if (end <= begin) return;
        const auto n = static_cast<std::uint32_t>(end - begin);
        for (unsigned i = 4; i-- > 0;) out.push_back(static_cast<std::byte>(n >> (8 * i)));
        for (auto i = begin; i < end; ++i) out.push_back(static_cast<std::byte>(data[i]));
    };
    std::size_t start = SIZE_MAX;
    for (std::size_t i = 0; i + 2 < size;) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) { if (start != SIZE_MAX) emit(start, i); start = i + 3; i += 3; }
        else ++i;
    }
    if (start != SIZE_MAX) emit(start, size);
}
enum class Fixture { Ready, Unavailable, Failed };
struct Options {
    unsigned width = 320, height = 192, frames = 48, fps = 30; bool ten_bit = false; unsigned gop = 24, b_frames = 2, bitrate = 8000000;
    unsigned transfer = 0; // 0 = encoder default (SDR), 16 = PQ, 18 = HLG; HDR also signals BT.2020 primaries/matrix.
    bool h264 = false;     // H.264 (High profile unless `main`) instead of HEVC.
    bool main = false;
    bool software = false; // Only software encoders (the Microsoft H.264 MFT) instead of hardware-first.
};
// Fills one row of 8-bit luma for `frame`; chroma is neutral. 10-bit input stores v << 8 (P010).
using RowFill = void (*)(unsigned frame, unsigned y, unsigned width, unsigned height, std::uint8_t* row);

inline Fixture Encode(const Options& o, RowFill fill, std::vector<Encoded>& stream, bool& b_frames_requested) {
    const GUID input_format = o.ten_bit ? MFVideoFormat_P010 : MFVideoFormat_NV12;
    const LONGLONG duration = 10000000 / o.fps;
    const GUID subtype = o.h264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC;
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, input_format}, coded{MFMediaType_Video, subtype};
    IMFActivate** activates = nullptr; UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, (o.software ? 0u : static_cast<UINT32>(MFT_ENUM_FLAG_HARDWARE)) | MFT_ENUM_FLAG_SYNCMFT |
                           MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &coded, &activates, &count);
    if (FAILED(hr) || !count) { CoTaskMemFree(activates); return Fixture::Unavailable; }
    IMFActivate* activate = activates[0]; activate->AddRef();
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    IMFTransform* encoder = nullptr; IMFMediaEventGenerator* events = nullptr;
    auto finish = [&](Fixture result) {
        ReleaseCom(events); ReleaseCom(encoder); activate->ShutdownObject(); activate->Release(); return result;
    };
    hr = activate->ActivateObject(IID_PPV_ARGS(&encoder));
    if (FAILED(hr)) { std::printf("Encoder activation failed: %08lx\n", static_cast<unsigned long>(hr)); return finish(Fixture::Unavailable); }
    UINT32 async = 0; IMFAttributes* attributes = nullptr;
    if (SUCCEEDED(encoder->GetAttributes(&attributes)) && attributes) {
        (void)attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        if (async) (void)attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        ReleaseCom(attributes);
    }
    ICodecAPI* codec = nullptr;
    if (SUCCEEDED(encoder->QueryInterface(IID_PPV_ARGS(&codec)))) {
        VARIANT v{}; v.vt = VT_UI4;
        v.ulVal = o.gop; (void)codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        v.ulVal = o.b_frames; b_frames_requested = SUCCEEDED(codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v));
        ReleaseCom(codec);
    }
    auto video_type = [&](const GUID& subtype, IMFMediaType*& type) {
        HRESULT r = MFCreateMediaType(&type);
        if (SUCCEEDED(r)) r = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(r)) r = type->SetGUID(MF_MT_SUBTYPE, subtype);
        if (SUCCEEDED(r)) r = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, o.width, o.height);
        if (SUCCEEDED(r)) r = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, o.fps, 1);
        if (SUCCEEDED(r)) r = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(r)) r = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(r) && o.transfer) {
            r = type->SetUINT32(MF_MT_TRANSFER_FUNCTION, o.transfer == 16 ? MFVideoTransFunc_2084 : MFVideoTransFunc_HLG);
            if (SUCCEEDED(r)) r = type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
            if (SUCCEEDED(r)) r = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_10);
            if (SUCCEEDED(r)) r = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        }
        return r;
    };
    IMFMediaType* type = nullptr;
    hr = video_type(subtype, type);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AVG_BITRATE, o.bitrate);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_MPEG2_PROFILE, o.h264 ? static_cast<UINT32>(o.main ? eAVEncH264VProfile_Main : eAVEncH264VProfile_High) :
                                                                  o.ten_bit ? static_cast<UINT32>(eAVEncH265VProfile_Main_420_10) :
                                                                              static_cast<UINT32>(eAVEncH265VProfile_Main_420_8));
    if (SUCCEEDED(hr)) hr = encoder->SetOutputType(0, type, 0);
    ReleaseCom(type);
    if (FAILED(hr)) { std::printf("Encoder output type rejected: %08lx\n", static_cast<unsigned long>(hr)); return finish(Fixture::Unavailable); }
    const UINT32 bytes_per_sample = o.ten_bit ? 2 : 1;
    hr = video_type(input_format, type);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_DEFAULT_STRIDE, o.width * bytes_per_sample);
    if (SUCCEEDED(hr)) hr = encoder->SetInputType(0, type, 0);
    ReleaseCom(type);
    if (FAILED(hr)) { std::printf("Encoder input type rejected: %08lx\n", static_cast<unsigned long>(hr)); return finish(Fixture::Unavailable); }
    if (async && FAILED(encoder->QueryInterface(IID_PPV_ARGS(&events)))) return finish(Fixture::Failed);
    (void)encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    (void)encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    unsigned sent = 0;
    std::vector<std::uint8_t> row(o.width);
    auto input = [&]() -> HRESULT {
        const DWORD size = o.width * o.height * 3 / 2 * bytes_per_sample;
        IMFSample* sample = nullptr; IMFMediaBuffer* buffer = nullptr; BYTE* data = nullptr;
        HRESULT r = MFCreateSample(&sample);
        if (SUCCEEDED(r)) r = MFCreateMemoryBuffer(size, &buffer);
        if (SUCCEEDED(r)) r = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(r)) {
            auto* wide = reinterpret_cast<std::uint16_t*>(data);
            for (unsigned y = 0; y < o.height; ++y) {
                fill(sent, y, o.width, o.height, row.data());
                if (o.ten_bit) for (unsigned x = 0; x < o.width; ++x) wide[static_cast<std::size_t>(y) * o.width + x] = static_cast<std::uint16_t>(row[x] << 8);
                else std::copy(row.begin(), row.end(), data + static_cast<std::size_t>(y) * o.width);
            }
            const std::size_t luma = static_cast<std::size_t>(o.width) * o.height;
            if (o.ten_bit) std::fill(wide + luma, wide + luma + luma / 2, static_cast<std::uint16_t>(128u << 8));
            else std::fill(data + luma, data + luma + luma / 2, static_cast<BYTE>(128));
            r = buffer->Unlock();
        }
        if (SUCCEEDED(r)) r = buffer->SetCurrentLength(size);
        if (SUCCEEDED(r)) r = sample->AddBuffer(buffer);
        if (SUCCEEDED(r)) r = sample->SetSampleTime(static_cast<LONGLONG>(sent) * duration);
        if (SUCCEEDED(r)) r = sample->SetSampleDuration(duration);
        if (SUCCEEDED(r)) r = encoder->ProcessInput(0, sample, 0);
        ReleaseCom(buffer); ReleaseCom(sample);
        if (SUCCEEDED(r)) ++sent;
        return r;
    };
    auto output = [&]() -> HRESULT {
        MFT_OUTPUT_STREAM_INFO info{}; HRESULT r = encoder->GetOutputStreamInfo(0, &info);
        if (FAILED(r)) return r;
        MFT_OUTPUT_DATA_BUFFER buffer{};
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            IMFMediaBuffer* memory = nullptr;
            r = MFCreateSample(&buffer.pSample);
            if (SUCCEEDED(r)) r = MFCreateMemoryBuffer((std::max)(info.cbSize, static_cast<DWORD>(o.width * o.height * 4)), &memory);
            if (SUCCEEDED(r)) r = buffer.pSample->AddBuffer(memory);
            ReleaseCom(memory);
            if (FAILED(r)) { ReleaseCom(buffer.pSample); return r; }
        }
        DWORD status = 0; r = encoder->ProcessOutput(0, 1, &buffer, &status);
        if (buffer.pEvents) buffer.pEvents->Release();
        if (r == MF_E_TRANSFORM_STREAM_CHANGE) {
            IMFMediaType* changed = nullptr;
            r = encoder->GetOutputAvailableType(0, 0, &changed);
            if (SUCCEEDED(r)) r = encoder->SetOutputType(0, changed, 0);
            ReleaseCom(changed); ReleaseCom(buffer.pSample);
            return SUCCEEDED(r) ? S_FALSE : r;
        }
        if (SUCCEEDED(r) && buffer.pSample) {
            Encoded frame; (void)buffer.pSample->GetSampleTime(&frame.pts);
            IMFMediaBuffer* contiguous = nullptr; BYTE* data = nullptr; DWORD length = 0;
            r = buffer.pSample->ConvertToContiguousBuffer(&contiguous);
            if (SUCCEEDED(r)) r = contiguous->Lock(&data, nullptr, &length);
            if (SUCCEEDED(r)) { AppendAnnexB(data, length, frame.au); (void)contiguous->Unlock(); }
            ReleaseCom(contiguous);
            if (SUCCEEDED(r) && !frame.au.empty()) stream.push_back(std::move(frame));
        }
        ReleaseCom(buffer.pSample);
        return r;
    };
    auto end_of_stream = [&] {
        (void)encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        return encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    };
    if (!async) {
        while (sent < o.frames && SUCCEEDED(hr)) {
            hr = input();
            while (SUCCEEDED(hr) && (hr = output()) != MF_E_TRANSFORM_NEED_MORE_INPUT) {}
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) hr = S_OK;
        }
        if (SUCCEEDED(hr)) hr = end_of_stream();
        while (SUCCEEDED(hr) && (hr = output()) != MF_E_TRANSFORM_NEED_MORE_INPUT) {}
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) hr = S_OK;
    } else {
        bool drained = false;
        const auto deadline = GetTickCount64() + 120000;
        while (!drained && SUCCEEDED(hr) && GetTickCount64() < deadline) {
            IMFMediaEvent* event = nullptr;
            hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) { hr = S_OK; Sleep(1); continue; }
            if (FAILED(hr)) break;
            MediaEventType kind = MEUnknown; HRESULT status = S_OK;
            (void)event->GetType(&kind); (void)event->GetStatus(&status); ReleaseCom(event);
            if (FAILED(status)) { hr = status; break; }
            if (kind == METransformNeedInput && sent < o.frames) { hr = input(); if (SUCCEEDED(hr) && sent == o.frames) hr = end_of_stream(); }
            else if (kind == METransformHaveOutput) { hr = output(); if (hr == S_FALSE) hr = S_OK; }
            else if (kind == METransformDrainComplete) drained = true;
        }
        if (SUCCEEDED(hr) && !drained) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    (void)encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    if (FAILED(hr) || stream.empty()) {
        std::printf("FAIL %s fixture encoding: %08lx, %zu access units\n", o.h264 ? "H.264" : "HEVC", static_cast<unsigned long>(hr), stream.size());
        return finish(Fixture::Failed);
    }
    // Some encoders publish parameter sets only in the media type; prepend them to the first access unit.
    bool has_sps = false;
    ForEachNal(stream.front().au, [&](std::span<const std::byte> nal) {
        const auto header = std::to_integer<unsigned>(nal[0]);
        has_sps = has_sps || (o.h264 ? (header & 31) == 7 : ((header >> 1) & 63) == 33);
    });
    if (!has_sps) {
        IMFMediaType* current = nullptr; UINT8* header = nullptr; UINT32 header_bytes = 0;
        if (SUCCEEDED(encoder->GetOutputCurrentType(0, &current)) &&
            SUCCEEDED(current->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &header, &header_bytes))) {
            std::vector<std::byte> prefix; AppendAnnexB(header, header_bytes, prefix);
            stream.front().au.insert(stream.front().au.begin(), prefix.begin(), prefix.end());
            CoTaskMemFree(header);
        }
        ReleaseCom(current);
    }
    return finish(Fixture::Ready);
}
}
