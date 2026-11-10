#include "media/platform/windows/Win32WasapiCapture.hpp"
#include <audioclientactivationparams.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cstring>

namespace media::platform::windows {
namespace {
// ActivateAudioInterfaceAsync's completion handler (it must be agile: Windows calls it on a thread of its own). It lives on
// Prepare's stack and Prepare waits for it, so its reference count frees nothing.
class Activation final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE done = nullptr;
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject)) *out = static_cast<IAgileObject*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override { (void)SetEvent(done); return S_OK; }
};
// The audio client for what this process tree renders (CaptureSource::ProcessLoopback).
HRESULT ActivateProcessLoopback(IAudioClient** client) noexcept {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation{}; activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params); activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    Activation handler; handler.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!handler.done) return HRESULT_FROM_WIN32(GetLastError());
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation, &handler, &operation);
    if (SUCCEEDED(hr) && WaitForSingleObject(handler.done, 5000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    HRESULT activated = E_FAIL; IUnknown* unknown = nullptr;
    if (SUCCEEDED(hr)) hr = operation->GetActivateResult(&activated, &unknown);
    if (SUCCEEDED(hr)) hr = activated;
    if (SUCCEEDED(hr) && unknown) hr = unknown->QueryInterface(IID_PPV_ARGS(client));
    if (unknown) unknown->Release();
    if (operation) operation->Release();
    CloseHandle(handler.done);
    return hr;
}
}
EndpointStatus Win32WasapiCapture::Result(HRESULT result) noexcept {
    error_ = result;
    if (SUCCEEDED(result)) return EndpointStatus::Ready;
    if (result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_RESOURCES_INVALIDATED || result == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        lost_ = true; return EndpointStatus::DeviceLost;
    }
    return EndpointStatus::Failed;
}
EndpointStatus Win32WasapiCapture::Prepare(core::LinearMemoryArena& arena, CaptureOptions options) noexcept {
    if (client_ || ring_.Capacity() || arena.IsSealed() || options.sample_rate < 8000 || options.sample_rate > 192000 ||
        !options.ring_frames || (options.ring_frames & (options.ring_frames - 1))) return EndpointStatus::InvalidArgument;
    const bool process = options.source == CaptureSource::ProcessLoopback, loopback = process || options.source == CaptureSource::Loopback;
    IMMDeviceEnumerator* enumerator = nullptr; IMMDevice* endpoint = nullptr; WAVEFORMATEX* mix = nullptr;
    if (process) {
        // A process loopback client has no mix format of its own (GetMixFormat is not implemented): the audio engine converts
        // what the process renders to the format asked, mono float32 at the asked rate.
        HRESULT hr = ActivateProcessLoopback(&client_);
        if (SUCCEEDED(hr)) { event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!event_) hr = HRESULT_FROM_WIN32(GetLastError()); }
        if (SUCCEEDED(hr)) {
            WAVEFORMATEXTENSIBLE wave{};
            wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wave.Format.nChannels = 1; wave.Format.nSamplesPerSec = options.sample_rate;
            wave.Format.wBitsPerSample = 32; wave.Format.nBlockAlign = 4; wave.Format.nAvgBytesPerSec = options.sample_rate * 4;
            wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wave.Samples.wValidBitsPerSample = 32; wave.dwChannelMask = SPEAKER_FRONT_CENTER; wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                     1000000, 0, &wave.Format, nullptr);
            converting_ = true; sample_rate_ = endpoint_rate_ = options.sample_rate; channels_ = endpoint_channels_ = 1;
        }
        if (SUCCEEDED(hr)) hr = client_->SetEventHandle(event_);
        if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&capture_));
        if (SUCCEEDED(hr) && !ring_.Initialize(arena, options.ring_frames, sizeof(float))) hr = E_OUTOFMEMORY;
        if (FAILED(hr)) { Close(); error_ = hr; return EndpointStatus::Failed; }
        stats_ = {}; lost_ = false; error_ = S_OK;
        return EndpointStatus::Ready;
    }
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = loopback ? enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint) :
                                       enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &endpoint);
    if (SUCCEEDED(hr)) hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    if (SUCCEEDED(hr)) hr = client_->GetMixFormat(&mix);
    if (SUCCEEDED(hr)) {
        endpoint_channels_ = mix->nChannels; endpoint_rate_ = mix->nSamplesPerSec;
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!event_) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) {
        // First choice: Windows converts to mono float32 at the asked rate. A 100 ms endpoint buffer bounds what one
        // Pump can find waiting.
        WAVEFORMATEXTENSIBLE wave{};
        wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wave.Format.nChannels = 1; wave.Format.nSamplesPerSec = options.sample_rate;
        wave.Format.wBitsPerSample = 32; wave.Format.nBlockAlign = 4; wave.Format.nAvgBytesPerSec = options.sample_rate * 4;
        wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wave.Samples.wValidBitsPerSample = 32; wave.dwChannelMask = SPEAKER_FRONT_CENTER; wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        const DWORD base = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        constexpr REFERENCE_TIME kBuffer = 1000000;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, base | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 kBuffer, 0, &wave.Format, nullptr);
        converting_ = SUCCEEDED(hr); sample_rate_ = options.sample_rate; channels_ = 1;
        if (!converting_) {
            // The endpoint's own format (always accepted): float32 only, averaged to mono in Write. A failed
            // Initialize leaves the client unusable, so a new one is activated.
            const bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
                reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            client_->Release(); client_ = nullptr;
            if (is_float && mix->wBitsPerSample == 32 && mix->nChannels && mix->nChannels <= 32) {
                hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
                if (SUCCEEDED(hr)) hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, base, kBuffer, 0, mix, nullptr);
                sample_rate_ = mix->nSamplesPerSec; channels_ = mix->nChannels;
            }
        }
    }
    if (SUCCEEDED(hr)) hr = client_->SetEventHandle(event_);
    if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (mix) CoTaskMemFree(mix);
    if (endpoint) endpoint->Release();
    if (enumerator) enumerator->Release();
    if (SUCCEEDED(hr) && !ring_.Initialize(arena, options.ring_frames, sizeof(float))) hr = E_OUTOFMEMORY;
    if (FAILED(hr)) { Close(); error_ = hr; return EndpointStatus::Failed; }
    stats_ = {}; lost_ = false; error_ = S_OK;
    return EndpointStatus::Ready;
}
EndpointStatus Win32WasapiCapture::Start() noexcept {
    if (!capture_ || running_ || lost_) return EndpointStatus::InvalidArgument;
    const auto started = Result(client_->Start());
    running_ = started == EndpointStatus::Ready;
    return started;
}
EndpointStatus Win32WasapiCapture::Stop() noexcept {
    if (!capture_ || !running_) return EndpointStatus::InvalidArgument;
    running_ = false;
    return Result(client_->Stop());
}
void Win32WasapiCapture::Write(const float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept {
    const float scale = 1.0f / static_cast<float>(channels);
    while (frames) {
        AudioWriteSpan span{};
        if (ring_.TryAcquire(frames, span) != EndpointStatus::Ready) { stats_.dropped += frames; return; } // Full: newest dropped.
        auto* out = reinterpret_cast<float*>(span.data);
        if (!interleaved) std::memset(out, 0, static_cast<std::size_t>(span.frames) * sizeof(float));
        else if (channels == 1) std::memcpy(out, interleaved, static_cast<std::size_t>(span.frames) * sizeof(float));
        else for (std::uint32_t i = 0; i < span.frames; ++i) {
            float sum = 0;
            for (std::uint32_t c = 0; c < channels; ++c) sum += interleaved[static_cast<std::size_t>(i) * channels + c];
            out[i] = sum * scale;
        }
        (void)ring_.Commit(span.lease, span.frames);
        stats_.frames += span.frames; frames -= span.frames;
        if (interleaved) interleaved += static_cast<std::size_t>(span.frames) * channels;
    }
}
EndpointStatus Win32WasapiCapture::Pump() noexcept {
    if (!running_ || lost_) return EndpointStatus::InvalidArgument;
    // The event says a packet is ready. A loopback stream raises it only while something renders, so the packet size
    // is asked for as well: both are non-blocking.
    (void)WaitForSingleObject(event_, 0);
    bool moved = false;
    for (;;) {
        UINT32 next = 0;
        HRESULT hr = capture_->GetNextPacketSize(&next);
        if (FAILED(hr)) return Result(hr);
        if (!next) break;
        BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 position = 0, qpc = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, &position, &qpc);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
        if (FAILED(hr)) return Result(hr);
        if (!stats_.packets) stats_.first_qpc_100ns = static_cast<std::int64_t>(qpc);
        ++stats_.packets;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++stats_.discontinuities;
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        if (silent) ++stats_.silent;
        Write(silent ? nullptr : reinterpret_cast<const float*>(data), frames, channels_);
        hr = capture_->ReleaseBuffer(frames);
        if (FAILED(hr)) return Result(hr);
        moved = true;
    }
    return moved ? EndpointStatus::Ready : EndpointStatus::WouldBlock;
}
std::uint32_t Win32WasapiCapture::Read(std::span<float> destination) noexcept {
    return ring_.ReadInto(std::as_writable_bytes(destination));
}
void Win32WasapiCapture::Close() noexcept {
    if (client_ && running_) (void)client_->Stop();
    running_ = false;
    if (capture_) { capture_->Release(); capture_ = nullptr; }
    if (client_) { client_->Release(); client_ = nullptr; }
    if (event_) { CloseHandle(event_); event_ = nullptr; }
}
}
