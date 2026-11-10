#include "media/platform/windows/Win32WasapiAudioSink.hpp"
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cwchar>

namespace media::platform::windows {
namespace {
constexpr std::int64_t kRetryInterval = 2000000; // 200 ms between attempts to leave the null endpoint.
}
HRESULT STDMETHODCALLTYPE Win32WasapiAudioSink::Notifications::QueryInterface(REFIID id, void** object) {
    if (!object) return E_POINTER;
    *object = id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient) ? static_cast<IMMNotificationClient*>(this) : nullptr;
    return *object ? S_OK : E_NOINTERFACE;
}
Win32WasapiAudioSink::~Win32WasapiAudioSink() noexcept { Close(); }
EndpointStatus Win32WasapiAudioSink::Result(HRESULT result) noexcept {
    error_.store(result);
    if (SUCCEEDED(result)) return EndpointStatus::Ready;
    const auto state = Invalidated(result) ? EndpointStatus::DeviceLost : EndpointStatus::Failed;
    status_.store(state); return state;
}
bool Win32WasapiAudioSink::Invalidated(HRESULT result) noexcept {
    return result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_RESOURCES_INVALIDATED || result == AUDCLNT_E_SERVICE_NOT_RUNNING;
}
std::int64_t Win32WasapiAudioSink::Now() const noexcept {
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    const auto f = static_cast<std::int64_t>(qpc_frequency_);
    return counter.QuadPart / f * 10000000 + counter.QuadPart % f * 10000000 / f;
}
EndpointStatus Win32WasapiAudioSink::Prepare(core::LinearMemoryArena& arena, WasapiOptions options) noexcept {
    if (client_ || enumerator_ || ring_.Capacity() || arena.IsSealed() || !options.format.sample_rate ||
        !options.format.channels || options.format.channels > 8 ||
        (options.format.encoding != AudioEncoding::Float32 && options.format.encoding != AudioEncoding::SignedPcm) ||
        (options.format.encoding == AudioEncoding::Float32 && options.format.bytes_per_sample != 4) ||
        (options.format.encoding == AudioEncoding::SignedPcm && options.format.bytes_per_sample != 2 &&
         options.format.bytes_per_sample != 3 && options.format.bytes_per_sample != 4) ||
        (options.endpoint_id && std::wcslen(options.endpoint_id) >= std::size(requested_id_))) return EndpointStatus::InvalidArgument;
    auto& wave = wave_; wave = {};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = options.format.channels;
    wave.Format.nSamplesPerSec = options.format.sample_rate;
    wave.Format.nBlockAlign = static_cast<WORD>(options.format.channels * options.format.bytes_per_sample);
    if (options.format.sample_rate > UINT32_MAX / wave.Format.nBlockAlign) return EndpointStatus::InvalidArgument;
    wave.Format.nAvgBytesPerSec = wave.Format.nSamplesPerSec * wave.Format.nBlockAlign;
    wave.Format.wBitsPerSample = static_cast<WORD>(options.format.bytes_per_sample * 8);
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.Samples.wValidBitsPerSample = wave.Format.wBitsPerSample;
    wave.dwChannelMask = options.channel_mask ? options.channel_mask : options.format.channels == 1 ? SPEAKER_FRONT_CENTER :
        options.format.channels == 2 ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT : options.format.channels == 6 ? KSAUDIO_SPEAKER_5POINT1 :
        options.format.channels == 8 ? KSAUDIO_SPEAKER_7POINT1_SURROUND : 0;
    if (std::popcount(wave.dwChannelMask) != options.format.channels) return EndpointStatus::InvalidArgument;
    wave.SubFormat = options.format.encoding == AudioEncoding::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || frequency.QuadPart > INT64_MAX / 10000000) return EndpointStatus::InvalidArgument;
    qpc_frequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
    allow_conversion_ = options.allow_conversion; requested_period_ = options.period_frames; ring_frames_ = options.ring_frames;
    follow_default_ = options.endpoint_id == nullptr;
    if (!follow_default_) (void)wcsncpy_s(requested_id_, options.endpoint_id, _TRUNCATE);
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
    if (SUCCEEDED(hr)) { event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!event_) hr = HRESULT_FROM_WIN32(GetLastError()); }
    if (FAILED(hr)) { Close(); return Result(hr); }
    bool invalid = false;
    hr = Open(invalid);
    if (invalid) { Close(); return EndpointStatus::InvalidArgument; }
    if (FAILED(hr)) { Close(); return Result(hr); }
    if (!ring_.Initialize(arena, options.ring_frames, wave.Format.nBlockAlign)) { Close(); return Result(E_OUTOFMEMORY); }
    // Following the default endpoint: Windows tells us when it changes (or a device comes and goes).
    if (follow_default_) registered_ = SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(&notifications_));
    notifications_.changed.store(false);
    format_ = options.format; status_.store(EndpointStatus::Ready); error_.store(S_OK);
    return EndpointStatus::Ready;
}
HRESULT Win32WasapiAudioSink::Open(bool& invalid) noexcept {
    invalid = false;
    if (refuse_until_ && Now() < refuse_until_) return AUDCLNT_E_DEVICE_IN_USE; // InjectInvalidation's hold.
    const bool replacement = ring_.Capacity() != 0; // After Prepare: any endpoint that plays this stream will do.
    IMMDevice* endpoint = nullptr; LPWSTR id = nullptr;
    HRESULT hr = follow_default_ ? enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint) : enumerator_->GetDevice(requested_id_, &endpoint);
    if (SUCCEEDED(hr)) hr = endpoint->GetId(&id);
    if (SUCCEEDED(hr)) (void)wcsncpy_s(endpoint_id_, id, _TRUNCATE);
    if (id) CoTaskMemFree(id);
    if (SUCCEEDED(hr)) hr = endpoint->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    if (endpoint) endpoint->Release();
    if (FAILED(hr)) { ReleaseClient(); return hr; }
    WAVEFORMATEX* closest = nullptr;
    hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &wave_.Format, &closest);
    if (closest) CoTaskMemFree(closest);
    converting_ = hr != S_OK && (allow_conversion_ || replacement) && (SUCCEEDED(hr) || hr == AUDCLNT_E_UNSUPPORTED_FORMAT);
    if (hr != S_OK && !converting_) { ReleaseClient(); return FAILED(hr) ? hr : AUDCLNT_E_UNSUPPORTED_FORMAT; }
    std::uint32_t period = 0;
    if (converting_) {
        // The engine converts to its mix format. Low-latency shared streams require the mix format, so this uses the
        // classic initialisation at the device's default period (typically 10 ms) and a three-period buffer.
        REFERENCE_TIME device_period = 0, minimum_period = 0;
        hr = client_->GetDevicePeriod(&device_period, &minimum_period);
        if (SUCCEEDED(hr)) hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 3 * device_period, 0, &wave_.Format, nullptr);
        period = SUCCEEDED(hr) ? static_cast<std::uint32_t>(static_cast<std::uint64_t>(device_period) * wave_.Format.nSamplesPerSec / 10000000) : 0;
        invalid = SUCCEEDED(hr) && (!period || ring_frames_ < period);
    } else {
        UINT32 default_period = 0, fundamental = 0, minimum = 0, maximum = 0;
        hr = client_->GetSharedModeEnginePeriod(&wave_.Format, &default_period, &fundamental, &minimum, &maximum);
        if (SUCCEEDED(hr)) {
            period = requested_period_ ? requested_period_ : minimum;
            // A replacement endpoint that cannot take the requested period runs at its own minimum.
            if (replacement && fundamental && (period < minimum || period > maximum || period % fundamental)) period = minimum;
            invalid = !fundamental || period < minimum || period > maximum || period % fundamental || ring_frames_ < period;
            if (!invalid) hr = client_->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, &wave_.Format, nullptr);
        }
    }
    if (invalid) { ReleaseClient(); return E_INVALIDARG; }
    UINT32 frames = 0;
    if (SUCCEEDED(hr)) hr = client_->SetEventHandle(event_);
    if (SUCCEEDED(hr)) hr = client_->GetBufferSize(&frames);
    if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&render_));
    if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&clock_));
    if (SUCCEEDED(hr)) hr = clock_->GetFrequency(&frequency_);
    if (SUCCEEDED(hr) && !frequency_) hr = E_FAIL;
    if (FAILED(hr)) { ReleaseClient(); return hr; }
    period_ = period; endpoint_frames_ = frames; (void)ResetEvent(event_);
    return S_OK;
}
void Win32WasapiAudioSink::ReleaseClient() noexcept {
    if (clock_) { clock_->Release(); clock_ = nullptr; }
    if (render_) { render_->Release(); render_ = nullptr; }
    if (client_) { client_->Release(); client_ = nullptr; }
    client_running_ = false;
}
std::uint64_t Win32WasapiAudioSink::NullPosition(std::int64_t now) const noexcept {
    const auto elapsed = running_ && now > null_start_ ? static_cast<std::uint64_t>(now - null_start_) : 0;
    return null_played_ + elapsed * format_.sample_rate / 10000000;
}
void Win32WasapiAudioSink::NullFill(std::int64_t now) noexcept {
    // The null endpoint plays exactly what the stream rate says has elapsed: queued PCM is consumed unread, and a
    // shortfall is silence (accounted like an endpoint underrun). Nothing is buffered ahead, so nothing is lost when
    // a real endpoint takes over.
    const auto played = NullPosition(now);
    if (played <= submitted_frames_) return;
    const auto frames = static_cast<std::uint32_t>((std::min)(played - submitted_frames_, std::uint64_t{UINT32_MAX}));
    const auto consumed = ring_.Skip(frames);
    if (consumed < frames) {
        silence_frames_.fetch_add(frames - consumed, std::memory_order_relaxed);
        last_silence_end_frame_ = submitted_frames_ + frames;
        if (!in_gap_) { ++generation_; in_gap_ = true; ++gaps_; gap_frames_ = 0; }
        gap_frames_ += frames - consumed;
    } else if (in_gap_) { ++generation_; in_gap_ = false; }
    submitted_frames_ += frames;
}
void Win32WasapiAudioSink::Detach() noexcept {
    ReleaseClient();
    // Everything handed to the endpoint is now behind the stream: frames it still held are skipped, not replayed.
    offset_frames_ += submitted_frames_; submitted_frames_ = last_silence_end_frame_ = null_played_ = 0; in_gap_ = false;
    null_start_ = Now(); retry_at_ = null_start_ + kRetryInterval;
    ++generation_;
}
void Win32WasapiAudioSink::Reattach() noexcept {
    if (reattaching_ || client_) return;
    reattaching_ = true;
    bool invalid = false;
    const HRESULT hr = Open(invalid);
    recovery_.last_open = hr;
    const auto now = Now();
    if (FAILED(hr)) {
        if (!detached_counted_) { ++recovery_.detachments; detached_counted_ = true; }
        retry_at_ = now + kRetryInterval; reattaching_ = false; return;
    }
    // The null endpoint plays until the replacement is ready, including the time Open took (170 ms for an endpoint
    // just released from exclusive mode): the media clock carries on at 1x instead of stepping back.
    NullFill(now);
    offset_frames_ += submitted_frames_; submitted_frames_ = last_silence_end_frame_ = null_played_ = 0; in_gap_ = false;
    ++generation_; ++recovery_.migrations; detached_counted_ = false;
    if (running_) {
        const auto filled = Fill(); // A replacement that fails at once detaches again (no nested retry).
        if (client_ && (filled == EndpointStatus::Ready || filled == EndpointStatus::WouldBlock)) {
            const HRESULT started = client_->Start();
            if (SUCCEEDED(started)) client_running_ = true;
            else if (Invalidated(started)) Lose(started);
            else (void)Result(started);
        }
    }
    reattaching_ = false;
}
bool Win32WasapiAudioSink::DefaultMoved() const noexcept {
    if (!follow_default_) return false;
    IMMDevice* device = nullptr; LPWSTR id = nullptr;
    const bool moved = SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) && SUCCEEDED(device->GetId(&id)) &&
                       std::wcscmp(id, endpoint_id_) != 0;
    if (id) CoTaskMemFree(id);
    if (device) device->Release();
    return moved;
}
void Win32WasapiAudioSink::Lose(HRESULT hr) noexcept {
    ++recovery_.losses; recovery_.last_loss = hr;
    Detach();
    // A new default endpoint (headphones unplugged, speakers now default) is opened at once. The same endpoint is
    // retried only after the interval: it may have been taken by an exclusive-mode application, which reopening at
    // once would fight, or its driver may be restarting.
    if (DefaultMoved()) Reattach();
}
void Win32WasapiAudioSink::FollowDefault() noexcept {
    if (!follow_default_) return;
    if (!client_) { retry_at_ = 0; return; } // Null endpoint: try the new default on this pump.
    if (!DefaultMoved()) return;
    // The stream moves to the new default (or, with none, to the null endpoint until one appears). The old endpoint
    // is stopped first; whatever it still held is skipped.
    ++recovery_.default_changes;
    if (client_running_) (void)client_->Stop();
    Detach(); Reattach();
}
HRESULT Win32WasapiAudioSink::QueryMix(EndpointMix& mix, const wchar_t* endpoint_id) noexcept {
    mix = {};
    IMMDeviceEnumerator* enumerator = nullptr; IMMDevice* endpoint = nullptr; IAudioClient* client = nullptr; WAVEFORMATEX* format = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = endpoint_id ? enumerator->GetDevice(endpoint_id, &endpoint) : enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint);
    if (SUCCEEDED(hr)) hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
    if (SUCCEEDED(hr)) {
        mix.sample_rate = format->nSamplesPerSec; mix.channels = format->nChannels;
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
            mix.channel_mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask;
        else mix.channel_mask = mix.channels == 1 ? SPEAKER_FRONT_CENTER : mix.channels == 2 ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT : 0;
    }
    if (format) CoTaskMemFree(format);
    if (client) client->Release();
    if (endpoint) endpoint->Release();
    if (enumerator) enumerator->Release();
    return hr;
}
EndpointStatus Win32WasapiAudioSink::TryAcquire(std::uint32_t frames, AudioWriteSpan& span) noexcept {
    const auto status = status_.load();
    if (status != EndpointStatus::Ready) { span = {}; return status; }
    return ring_.TryAcquire(frames, span);
}
EndpointStatus Win32WasapiAudioSink::Commit(std::uint64_t lease, std::uint32_t frames) noexcept {
    return ring_.Commit(lease, frames);
}
EndpointStatus Win32WasapiAudioSink::Fill() noexcept {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (injected_loss_ != S_OK) {
        // Fault test: this call's result is the injected one, and endpoints refuse to open for the hold.
        hr = injected_loss_; injected_loss_ = S_OK; ++recovery_.injected;
        refuse_until_ = Now() + static_cast<std::int64_t>(injected_refuse_ms_) * 10000;
    }
    if (Invalidated(hr)) { Lose(hr); return EndpointStatus::Ready; }
    if (FAILED(hr)) return Result(hr);
    if (padding > endpoint_frames_) return Result(E_UNEXPECTED);
    const auto frames = endpoint_frames_ - padding;
    if (!frames) return EndpointStatus::WouldBlock;
    if (submitted_frames_ > UINT64_MAX - frames || generation_ == UINT32_MAX) return Result(E_UNEXPECTED);
    BYTE* destination = nullptr;
    hr = render_->GetBuffer(frames, &destination);
    if (Invalidated(hr)) { Lose(hr); return EndpointStatus::Ready; }
    if (FAILED(hr)) return Result(hr);
    const auto stride = static_cast<std::uint32_t>(format_.channels) * format_.bytes_per_sample;
    const auto copied = injected_gap_ ? 0u : ring_.ReadInto({reinterpret_cast<std::byte*>(destination), static_cast<std::size_t>(frames) * stride});
    if (gain_ && gain_controls_ && format_.encoding == AudioEncoding::Float32 && copied) {
        gain_->Apply(*gain_controls_); gain_->Process(reinterpret_cast<float*>(destination), copied); // Volume and mute, ramped.
    }
    // Short of PCM: silence only as far as keeps half the buffer queued (none while that much still is), the rest of the space
    // left for the PCM the producer brings by the next period.
    const std::uint32_t queued = padding + copied, keep = endpoint_frames_ / 2;
    const std::uint32_t silence = injected_gap_ ? (std::min)(injected_gap_, frames) : copied < frames && queued < keep ? (std::min)(keep - queued, frames - copied) : 0u;
    const std::uint32_t written = copied + silence;
    injected_gap_ = 0;
    if (silence) {
        std::memset(destination + static_cast<std::size_t>(copied) * stride, 0, static_cast<std::size_t>(silence) * stride);
        silence_frames_.fetch_add(silence, std::memory_order_relaxed);
        last_silence_end_frame_ = submitted_frames_ + written;
        if (!in_gap_) { ++generation_; in_gap_ = true; ++gaps_; gap_frames_ = 0; }
        gap_frames_ += silence;
    } else if (in_gap_ && copied) { ++generation_; in_gap_ = false; }
    hr = render_->ReleaseBuffer(written, copied == 0 && silence ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    // Frames taken from the ring are handed over even if the endpoint died meanwhile: they are skipped with it.
    if (SUCCEEDED(hr) || Invalidated(hr)) submitted_frames_ += written;
    if (Invalidated(hr)) { Lose(hr); return EndpointStatus::Ready; }
    return Result(hr);
}
EndpointStatus Win32WasapiAudioSink::Start() noexcept {
    if (!ring_.Capacity() || running_ || status_.load() != EndpointStatus::Ready) return EndpointStatus::InvalidArgument;
    if (!client_ && Now() >= retry_at_) Reattach(); // Stopped on the null endpoint: a real one may be back.
    running_ = true;
    if (!client_) { null_start_ = Now(); return EndpointStatus::Ready; }
    const auto filled = Fill(); // A loss here moves the stream (and starts the replacement, as running_ is set).
    if (filled != EndpointStatus::Ready && filled != EndpointStatus::WouldBlock) { running_ = false; return filled; }
    if (!client_ || client_running_) return EndpointStatus::Ready;
    const HRESULT hr = client_->Start();
    if (Invalidated(hr)) { Lose(hr); return EndpointStatus::Ready; }
    const auto result = Result(hr);
    client_running_ = result == EndpointStatus::Ready; running_ = client_running_;
    return result;
}
EndpointStatus Win32WasapiAudioSink::Pump(bool signaled) noexcept {
    if (!running_ || status_.load() != EndpointStatus::Ready) return EndpointStatus::InvalidArgument;
    const auto now = Now();
    if (notifications_.changed.exchange(false)) FollowDefault();
    if (!client_ && now >= retry_at_) Reattach();
    if (!client_) { NullFill(now); return EndpointStatus::Ready; }
    if (injected_loss_ != S_OK) return Fill(); // Raised on this pump, whether or not the endpoint asked for data.
    const auto signal = signaled ? WAIT_OBJECT_0 : WaitForSingleObject(event_, 0);
    if (signal == WAIT_TIMEOUT) return EndpointStatus::WouldBlock;
    if (signal != WAIT_OBJECT_0) return Result(HRESULT_FROM_WIN32(GetLastError()));
    return Fill();
}
EndpointStatus Win32WasapiAudioSink::Clock(AudioClockSample& sample) noexcept {
    sample = {};
    if (!running_ || status_.load() != EndpointStatus::Ready) return EndpointStatus::InvalidArgument;
    if (client_) {
        UINT64 position = 0, qpc = 0;
        const auto hr = clock_->GetPosition(&position, &qpc);
        if (SUCCEEDED(hr)) {
            if (qpc > INT64_MAX) return Result(E_UNEXPECTED);
            // Filling a later PCM buffer does not mean earlier queued silence has played.
            // Keep the tuple unusable for media anchoring until the hardware passes it.
            const bool silence_played = !last_silence_end_frame_ ||
                static_cast<long double>(position) / static_cast<long double>(frequency_) >
                static_cast<long double>(last_silence_end_frame_) / format_.sample_rate;
            sample = {position, frequency_, static_cast<std::int64_t>(qpc), hr == S_OK && !in_gap_ && silence_played, generation_};
            return EndpointStatus::Ready;
        }
        if (!Invalidated(hr)) return Result(hr);
        Lose(hr);
        // A replacement endpoint has not played anything yet: the next observation is its first usable one.
        if (client_) { sample = {0, frequency_, Now(), false, generation_}; return EndpointStatus::Ready; }
    }
    // Null endpoint: exact by construction, on the same monotonic clock.
    const auto now = Now(); const auto position = NullPosition(now);
    const bool silence_played = !last_silence_end_frame_ || position > last_silence_end_frame_;
    sample = {position, format_.sample_rate, now, !in_gap_ && silence_played, generation_};
    return EndpointStatus::Ready;
}
EndpointStatus Win32WasapiAudioSink::Stop() noexcept {
    if (!ring_.Capacity()) return EndpointStatus::InvalidArgument;
    if (running_ && !client_) null_played_ = NullPosition(Now());
    running_ = false;
    HRESULT hr = S_OK;
    if (client_ && client_running_) { hr = client_->Stop(); client_running_ = false; }
    if (Invalidated(hr)) { Lose(hr); hr = S_OK; }
    if (generation_ == UINT32_MAX) return Result(E_UNEXPECTED);
    ++generation_;
    return Result(hr);
}
EndpointStatus Win32WasapiAudioSink::Reset() noexcept {
    if (!ring_.Capacity() || status_.load() != EndpointStatus::Ready) return EndpointStatus::InvalidArgument;
    if (running_) { const auto stopped = Stop(); if (stopped != EndpointStatus::Ready) return stopped; }
    // Stopped stream: IAudioClient::Reset drops the endpoint's queued frames and restarts its position at 0.
    if (client_) {
        const auto hr = client_->Reset();
        if (Invalidated(hr)) Lose(hr);
        else if (FAILED(hr)) return Result(hr);
    }
    (void)ring_.Discard();
    if (generation_ == UINT32_MAX) return Result(E_UNEXPECTED);
    ++generation_; submitted_frames_ = last_silence_end_frame_ = offset_frames_ = null_played_ = 0; in_gap_ = false;
    silence_frames_.store(0, std::memory_order_relaxed);
    return EndpointStatus::Ready;
}
void Win32WasapiAudioSink::Close() noexcept {
    status_.store(EndpointStatus::Failed);
    // Unregistering waits for callbacks in progress; the embedded notification object is not used afterwards.
    if (registered_) { (void)enumerator_->UnregisterEndpointNotificationCallback(&notifications_); registered_ = false; }
    if (client_ && client_running_) (void)client_->Stop();
    running_ = false;
    ReleaseClient();
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    if (event_) { CloseHandle(event_); event_ = nullptr; }
}
}
