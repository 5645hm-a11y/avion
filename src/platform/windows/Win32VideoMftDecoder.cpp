#include "media/platform/windows/Win32VideoMftDecoder.hpp"
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <algorithm>
#include <cstring>

namespace media::platform::windows {
namespace {
template <class T> void SafeRelease(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
constexpr UINT32 kSoftware = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
// The first software (not hardware, not asynchronous) decoder of `subtype`, activated; its name into `name`.
IMFTransform* Find(const GUID& subtype, wchar_t* name, unsigned capacity, HRESULT& error) noexcept {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, subtype};
    IMFActivate** list = nullptr; UINT32 count = 0;
    error = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, kSoftware, &input, nullptr, &list, &count);
    IMFTransform* transform = nullptr;
    for (UINT32 i = 0; SUCCEEDED(error) && i < count && !transform; ++i) {
        UINT32 flags = 0; (void)list[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &flags);
        if (flags & (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT)) continue;
        if (name && capacity) { UINT32 length = 0; if (FAILED(list[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, capacity, &length))) name[0] = 0; }
        if (FAILED(list[i]->ActivateObject(IID_PPV_ARGS(&transform)))) transform = nullptr;
    }
    for (UINT32 i = 0; i < count; ++i) list[i]->Release();
    CoTaskMemFree(list);
    if (SUCCEEDED(error) && !transform) error = MF_E_TOPO_CODEC_NOT_FOUND;
    return transform;
}
}

Win32VideoMftDecoder::~Win32VideoMftDecoder() noexcept { Close(); }

bool Win32VideoMftDecoder::Available(const GUID& subtype, wchar_t* name, unsigned capacity) noexcept {
    HRESULT error = S_OK;
    IMFTransform* transform = Find(subtype, name, capacity, error);
    const bool found = transform != nullptr;
    SafeRelease(transform);
    return found;
}

bool Win32VideoMftDecoder::Prepare(const GUID& subtype, unsigned width, unsigned height, bool ten_bit, std::uint32_t max_input_bytes) noexcept {
    if (transform_ || !width || !height || !max_input_bytes) return false;
    subtype_ = subtype; ten_bit_ = ten_bit;
    transform_ = Find(subtype, name_, static_cast<unsigned>(std::size(name_)), error_);
    if (!transform_) return false;
    IMFMediaType* input = nullptr;
    error_ = MFCreateMediaType(&input);
    if (SUCCEEDED(error_)) error_ = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(error_)) error_ = input->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(error_)) error_ = MFSetAttributeSize(input, MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(error_)) error_ = transform_->SetInputType(0, input, 0);
    SafeRelease(input);
    if (FAILED(error_) || !Output()) return false;
    // Its decode threads: half the logical processors at most, so that a burst of decoding (it decodes ahead while surfaces are
    // free) leaves the presentation and audio threads their cores. Advisory: a transform without ICodecAPI decides alone.
    ICodecAPI* codec = nullptr;
    if (SUCCEEDED(transform_->QueryInterface(IID_PPV_ARGS(&codec)))) {
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        VARIANT threads{}; threads.vt = VT_UI4; threads.ulVal = (std::max)(2ul, static_cast<unsigned long>(system.dwNumberOfProcessors / 2));
        (void)codec->SetValue(&CODECAPI_AVDecNumWorkerThreads, &threads);
        SafeRelease(codec);
    }
    MFT_INPUT_STREAM_INFO info{};
    error_ = transform_->GetInputStreamInfo(0, &info);
    if (FAILED(error_)) return false;
    for (auto& slot : inputs_) {
        error_ = MFCreateSample(&slot.storage);
        if (SUCCEEDED(error_)) error_ = MFCreateAlignedMemoryBuffer(max_input_bytes, (std::max)(info.cbAlignment, DWORD{16}) - 1, &slot.buffer);
        if (SUCCEEDED(error_)) error_ = slot.AddBuffer(slot.buffer);
        if (SUCCEEDED(error_)) error_ = slot.SetSampleTime(0);
        if (SUCCEEDED(error_)) error_ = slot.SetSampleDuration(0);
        if (FAILED(error_)) return false;
    }
    error_ = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(error_)) error_ = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return SUCCEEDED(error_);
}

bool Win32VideoMftDecoder::Output() noexcept {
    const GUID wanted = ten_bit_ ? MFVideoFormat_P010 : MFVideoFormat_NV12;
    IMFMediaType* chosen = nullptr;
    for (DWORD i = 0; i < 64 && !chosen; ++i) {
        IMFMediaType* available = nullptr;
        if (FAILED(transform_->GetOutputAvailableType(0, i, &available))) break;
        GUID subtype{}; (void)available->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == wanted) chosen = available; else available->Release();
    }
    if (!chosen) { error_ = MF_E_INVALIDMEDIATYPE; return false; }
    error_ = transform_->SetOutputType(0, chosen, 0);
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(error_)) error_ = MFGetAttributeSize(chosen, MF_MT_FRAME_SIZE, &w, &h);
    if (SUCCEEDED(error_)) {
        frame_width_ = w; frame_height_ = h; show_width_ = w; show_height_ = h; show_left_ = show_top_ = 0;
        UINT32 stride = 0;
        stride_ = SUCCEEDED(chosen->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<LONG>(static_cast<INT32>(stride)) : static_cast<LONG>(w * (ten_bit_ ? 2 : 1));
        MFVideoArea area{}; UINT32 bytes = 0;
        if (SUCCEEDED(chosen->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &bytes)) && bytes == sizeof(area) &&
            area.Area.cx > 0 && area.Area.cy > 0 && area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
            static_cast<unsigned>(area.OffsetX.value + area.Area.cx) <= w && static_cast<unsigned>(area.OffsetY.value + area.Area.cy) <= h) {
            show_left_ = static_cast<unsigned>(area.OffsetX.value) & ~1u; show_top_ = static_cast<unsigned>(area.OffsetY.value) & ~1u;
            show_width_ = static_cast<unsigned>(area.Area.cx); show_height_ = static_cast<unsigned>(area.Area.cy);
        }
    }
    SafeRelease(chosen);
    return SUCCEEDED(error_);
}

Win32VideoMftDecoder::Result Win32VideoMftDecoder::Decode(std::span<const std::byte> unit, std::int64_t pts, std::int64_t duration) noexcept {
    if (!transform_ || unit.empty()) return Result::Failed;
    // A slot is free when the transform holds neither its sample nor its buffer: the AV1 extension's decode threads keep the
    // buffer (not the sample) of the units they still read, about ten of them before the first picture.
    const auto free = [](NativeSampleLease& slot) noexcept {
        if (!slot.Available()) return false;
        slot.buffer->AddRef();
        return slot.buffer->Release() == 2; // Ours and the sample's.
    };
    NativeSampleLease* input = nullptr;
    for (auto& slot : inputs_) if (free(slot)) { input = &slot; break; }
    if (!input) return Result::Full; // The transform still holds every input sample.
    DWORD capacity = 0; error_ = input->buffer->GetMaxLength(&capacity);
    if (FAILED(error_) || unit.size() > capacity) return Result::Failed;
    BYTE* data = nullptr; error_ = input->buffer->Lock(&data, nullptr, nullptr);
    if (FAILED(error_)) return Result::Failed;
    std::memcpy(data, unit.data(), unit.size()); (void)input->buffer->Unlock();
    error_ = input->buffer->SetCurrentLength(static_cast<DWORD>(unit.size()));
    if (SUCCEEDED(error_)) error_ = input->SetSampleTime(pts);
    if (SUCCEEDED(error_)) error_ = input->SetSampleDuration(duration);
    if (SUCCEEDED(error_)) error_ = transform_->ProcessInput(0, input, 0);
    if (error_ == MF_E_NOTACCEPTING) { error_ = S_OK; return Result::Full; }
    return SUCCEEDED(error_) ? Result::Ready : Result::Failed;
}

Win32VideoMftDecoder::Result Win32VideoMftDecoder::Next(UploadPicture& picture) noexcept {
    if (!transform_ || current_) return Result::Failed;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        MFT_OUTPUT_DATA_BUFFER output{0, nullptr, 0, nullptr}; DWORD status = 0;
        error_ = transform_->ProcessOutput(0, 1, &output, &status);
        SafeRelease(output.pEvents);
        if (error_ == MF_E_TRANSFORM_STREAM_CHANGE) { SafeRelease(output.pSample); if (!Output()) return Result::Failed; continue; }
        if (error_ == MF_E_TRANSFORM_NEED_MORE_INPUT) { error_ = S_OK; drained_ = draining_; return Result::NeedInput; }
        if (FAILED(error_) || !output.pSample) { SafeRelease(output.pSample); return Result::Failed; }
        current_ = output.pSample;
        BYTE* scan0 = nullptr; LONG pitch = 0;
        error_ = current_->GetBufferByIndex(0, &buffer_);
        if (SUCCEEDED(error_) && SUCCEEDED(buffer_->QueryInterface(IID_PPV_ARGS(&plane_)))) error_ = plane_->Lock2D(&scan0, &pitch);
        else if (SUCCEEDED(error_)) { error_ = buffer_->Lock(&scan0, nullptr, nullptr); pitch = stride_; }
        if (FAILED(error_) || !scan0 || pitch <= 0) {
            // Not locked (or upside down): nothing to unlock.
            SafeRelease(plane_); SafeRelease(buffer_); SafeRelease(current_); if (SUCCEEDED(error_)) error_ = E_UNEXPECTED;
            return Result::Failed;
        }
        const std::size_t sample = ten_bit_ ? 2 : 1;
        picture.planes[0] = reinterpret_cast<const std::byte*>(scan0) + std::size_t{show_top_} * static_cast<std::size_t>(pitch) + show_left_ * sample;
        picture.planes[1] = reinterpret_cast<const std::byte*>(scan0) + std::size_t{frame_height_} * static_cast<std::size_t>(pitch) +
                            std::size_t{show_top_ / 2} * static_cast<std::size_t>(pitch) + show_left_ * sample;
        picture.pitches[0] = picture.pitches[1] = static_cast<std::uint32_t>(pitch);
        picture.width = show_width_; picture.height = show_height_;
        LONGLONG time = 0, length = 0;
        picture.pts_100ns = SUCCEEDED(current_->GetSampleTime(&time)) ? time : 0;
        picture.duration_100ns = SUCCEEDED(current_->GetSampleDuration(&length)) && length > 0 ? length : 0;
        ++pictures_;
        return Result::Picture;
    }
    return Result::Failed;
}

void Win32VideoMftDecoder::Release() noexcept {
    if (!current_) return;
    if (plane_) (void)plane_->Unlock2D(); else if (buffer_) (void)buffer_->Unlock();
    SafeRelease(plane_); SafeRelease(buffer_); SafeRelease(current_);
}

bool Win32VideoMftDecoder::Drain() noexcept {
    if (!transform_) return false;
    if (draining_) return true;
    error_ = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (SUCCEEDED(error_)) error_ = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    draining_ = SUCCEEDED(error_);
    return draining_;
}

bool Win32VideoMftDecoder::Flush() noexcept {
    if (!transform_) return false;
    Release();
    error_ = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (SUCCEEDED(error_)) error_ = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    draining_ = drained_ = false;
    return SUCCEEDED(error_);
}

void Win32VideoMftDecoder::Close() noexcept {
    Release();
    if (transform_) {
        (void)transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        (void)transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    SafeRelease(transform_);
    for (auto& input : inputs_) input.Close();
    draining_ = drained_ = false; pictures_ = 0;
}
} // namespace media::platform::windows
