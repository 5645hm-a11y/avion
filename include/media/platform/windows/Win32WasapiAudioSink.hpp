#pragma once
#include "media/audio/PcmRingBuffer.hpp"
#include "media/audio/DspProcessor.hpp"
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <atomic>

namespace media::platform::windows {
struct WasapiOptions {
    AudioFormat format{48000, 2, 4, AudioEncoding::Float32};
    std::uint32_t ring_frames = 16384;
    std::uint32_t period_frames = 0; // Zero selects the native minimum.
    std::uint32_t channel_mask = 0;       // 0: mono FC, stereo FL FR, 6 = 5.1 (0x3F), 8 = 7.1 (0x63F).
    const wchar_t* endpoint_id = nullptr; // Null selects default multimedia output (and follows it when it changes).
    // A format the endpoint's shared mix cannot take natively (e.g. 5.1 on stereo headphones) is opened with
    // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM: Windows converts channels/rate. Off: such formats fail with
    // AUDCLNT_E_UNSUPPORTED_FORMAT. A replacement endpoint after a device loss always allows conversion.
    bool allow_conversion = false;
};
// The shared-mode mix format of an endpoint: what it plays natively.
struct EndpointMix { std::uint32_t sample_rate = 0, channels = 0, channel_mask = 0; };
// Endpoint faults absorbed by the sink (endpoint owner; read by it, or after it stopped).
struct EndpointRecovery {
    unsigned losses = 0;          // Invalidated streams (AUDCLNT_E_DEVICE_INVALIDATED, ..._RESOURCES_INVALIDATED, ...).
    unsigned migrations = 0;      // Streams opened on a replacement endpoint (after a loss or a default change).
    unsigned detachments = 0;     // Losses after which no endpoint could be opened: the null endpoint ran instead.
    unsigned default_changes = 0; // Default render endpoint changes the stream followed.
    HRESULT last_loss = S_OK;     // Why the latest stream was invalidated.
    HRESULT last_open = S_OK;     // Latest attempt to open a replacement (e.g. AUDCLNT_E_DEVICE_IN_USE).
    unsigned injected = 0;        // Of the losses, raised by InjectInvalidation (fault tests) rather than by Windows.
};
// Shared-mode WASAPI render stream with endpoint fault recovery. An invalidated stream (headphones unplugged,
// Bluetooth dropped, an exclusive-mode application, driver restart, audio service restart) is not an error: the sink
// releases the client, keeps its PCM ring, and reopens the default endpoint in the same format (Windows converts if
// the new endpoint's mix differs). While no endpoint can be opened, a null endpoint consumes PCM at the stream rate
// on the monotonic clock, so playback and its clock continue silently; reopening is retried every 200 ms and at once
// when Windows reports a new default device (IMMNotificationClient). A default-device change also moves a healthy
// stream. Each endpoint change starts a new clock generation. Frames queued in a lost endpoint (at most its buffer)
// are skipped, never replayed: EndpointOffsetFrames keeps the device-position-to-media mapping exact.
class Win32WasapiAudioSink final : public IAudioSink {
public:
    Win32WasapiAudioSink() noexcept = default;
    ~Win32WasapiAudioSink() noexcept override;
    Win32WasapiAudioSink(const Win32WasapiAudioSink&) = delete;
    Win32WasapiAudioSink& operator=(const Win32WasapiAudioSink&) = delete;
    // Prepare, Start, Pump, Clock, Stop and Close belong to the same COM-initialized
    // endpoint owner. TryAcquire/Commit belong to one separate PCM producer.
    [[nodiscard]] EndpointStatus Prepare(core::LinearMemoryArena&, WasapiOptions = {}) noexcept;
    // Setup only (COM-initialised thread): the default (or given) render endpoint's mix format.
    [[nodiscard]] static HRESULT QueryMix(EndpointMix&, const wchar_t* endpoint_id = nullptr) noexcept;
    [[nodiscard]] bool Converting() const noexcept { return converting_; } // Opened through AUTOCONVERTPCM.
    [[nodiscard]] EndpointStatus Start() noexcept;
    [[nodiscard]] EndpointStatus Stop() noexcept;
    // Seek flush on the endpoint owner, with the PCM producer quiescent: stops if running, discards the
    // endpoint buffer and the PCM ring, and restarts device position and silence accounting at zero.
    // Start() then primes it again like a new stream.
    [[nodiscard]] EndpointStatus Reset() noexcept;
    void Close() noexcept; // Quiescent teardown only, all producer activity stopped.
    // Polls the endpoint's event with timeout zero; `signaled`: the caller's own wait on Wake() has just consumed it.
    [[nodiscard]] EndpointStatus Pump(bool signaled = false) noexcept;
    // The endpoint's period event (auto-reset), for an owner that waits instead of polling (docs/session.md, "Low-power
    // pacing"); nullptr while no endpoint is attached (the null endpoint is filled by time).
    [[nodiscard]] HANDLE Wake() const noexcept { return client_ ? event_ : nullptr; }
    [[nodiscard]] AudioFormat Format() const noexcept override { return format_; }
    [[nodiscard]] EndpointStatus TryAcquire(std::uint32_t, AudioWriteSpan&) noexcept override;
    [[nodiscard]] EndpointStatus Commit(std::uint64_t, std::uint32_t) noexcept override;
    [[nodiscard]] EndpointStatus Clock(AudioClockSample&) noexcept override;
    [[nodiscard]] HRESULT LastError() const noexcept { return error_.load(); }
    [[nodiscard]] std::uint32_t PeriodFrames() const noexcept { return period_; }
    [[nodiscard]] std::uint32_t EndpointFrames() const noexcept { return endpoint_frames_; }
    [[nodiscard]] std::uint32_t QueuedFrames() const noexcept { return ring_.QueuedFrames(); } // Endpoint owner only.
    [[nodiscard]] std::uint64_t SilenceFrames() const noexcept { return silence_frames_.load(); }
    // Underrun gaps (docs/session.md, "The audio lead", 2026-10-30), endpoint owner: how many began (each a new clock generation,
    // and another when it ends), whether one is going on, and the silence written in the current or last one. Short of PCM, the
    // sink writes silence only as far as keeps half the endpoint's buffer queued (none while that much still is): the producer
    // keeps the rest of a period to catch up before anything is lost, and a hiccup costs the silence it really needs.
    [[nodiscard]] std::uint32_t Gaps() const noexcept { return gaps_; }
    [[nodiscard]] bool InGap() const noexcept { return in_gap_; }
    [[nodiscard]] std::uint64_t GapFrames() const noexcept { return gap_frames_; }
    // Frames (at the stream rate, since the last Reset) handed to endpoints that have since been replaced. The media
    // position of a clock sample is EndpointOffsetFrames + position / frequency * rate - SilenceFrames.
    [[nodiscard]] std::uint64_t EndpointOffsetFrames() const noexcept { return offset_frames_; }
    [[nodiscard]] bool Attached() const noexcept { return client_ != nullptr; } // False: the null endpoint plays.
    [[nodiscard]] const EndpointRecovery& Recovery() const noexcept { return recovery_; }
    // Endpoint owner, setup: volume and mute (`controls`) applied by `gain` to float PCM as it is copied from the ring
    // into the endpoint buffer, so a change is heard within the endpoint's buffer. Null disables it.
    void SetOutputGain(audio::OutputGain* gain, const audio::DspControls* controls) noexcept { gain_ = gain; gain_controls_ = controls; }
    // Endpoint owner, fault tests (docs/recovery.md): the next Pump treats the stream as invalidated with `loss` (e.g.
    // AUDCLNT_E_DEVICE_INVALIDATED), exactly where GetCurrentPadding would report it, and opening any endpoint is then
    // refused with AUDCLNT_E_DEVICE_IN_USE for `refuse_ms`: what an exclusive-mode application holding the device does,
    // without taking the real device from the other applications on the machine. Everything after the injected result
    // is the production path (release, null endpoint, retries, reopen); only the two results are replaced.
    void InjectInvalidation(HRESULT loss, std::uint32_t refuse_ms) noexcept { injected_loss_ = loss; injected_refuse_ms_ = refuse_ms; }
    // Endpoint owner, the audio lead's test (docs/session.md, "The audio lead"): the next fill takes nothing from the PCM ring and
    // writes `frames` of silence instead (at most the free space), as a producer a period late would leave it: a gap of that
    // length in the stream, the PCM after it delayed by as much. Everything else is the production path.
    void InjectGap(std::uint32_t frames) noexcept { injected_gap_ = frames; }
private:
    // Endpoint notifications arrive on Windows' threads: they only raise a flag for the endpoint owner.
    class Notifications final : public IMMNotificationClient {
    public:
        std::atomic<bool> changed{false};
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; } // Embedded in the sink; unregistered before it dies.
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { changed.store(true); return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { changed.store(true); return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { changed.store(true); return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
            if (flow == eRender && role == eMultimedia) changed.store(true);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    };
    [[nodiscard]] EndpointStatus Result(HRESULT) noexcept;
    [[nodiscard]] EndpointStatus Fill() noexcept;
    [[nodiscard]] static bool Invalidated(HRESULT) noexcept;
    [[nodiscard]] HRESULT Open(bool& invalid) noexcept; // Client on the target endpoint in wave_; nothing on failure.
    void ReleaseClient() noexcept;
    void Detach() noexcept;       // Current endpoint gone: its frames become offset, the null endpoint takes over.
    void Reattach() noexcept;     // Try a real endpoint again (from the null endpoint).
    void Lose(HRESULT) noexcept;  // Stream invalidated: detach, then try a replacement at once.
    void FollowDefault() noexcept;
    [[nodiscard]] bool DefaultMoved() const noexcept; // The default endpoint is no longer the one last opened.
    void NullFill(std::int64_t now) noexcept;
    [[nodiscard]] std::uint64_t NullPosition(std::int64_t now) const noexcept;
    [[nodiscard]] std::int64_t Now() const noexcept; // Monotonic 100 ns (the IAudioClock QPC domain).
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IAudioClient3* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    IAudioClock* clock_ = nullptr;
    HANDLE event_ = nullptr;
    Notifications notifications_;
    audio::PcmRingBuffer ring_;
    AudioFormat format_{};
    WAVEFORMATEXTENSIBLE wave_{};
    wchar_t requested_id_[256]{}, endpoint_id_[256]{};
    std::uint64_t frequency_ = 0, qpc_frequency_ = 0;
    std::uint64_t submitted_frames_ = 0, last_silence_end_frame_ = 0, offset_frames_ = 0;
    std::uint64_t null_played_ = 0;  // Null endpoint: frames played before the current run.
    std::int64_t null_start_ = 0, retry_at_ = 0;
    HRESULT injected_loss_ = S_OK;          // InjectInvalidation: raised by the next Fill.
    std::uint32_t injected_refuse_ms_ = 0;
    std::int64_t refuse_until_ = 0;         // Opens are refused until then (Now()).
    std::uint32_t period_ = 0, requested_period_ = 0, ring_frames_ = 0, endpoint_frames_ = 0, generation_ = 1;
    // running_: started by the owner (Start/Stop). client_running_: the current endpoint client is started.
    bool running_ = false, client_running_ = false, in_gap_ = false, converting_ = false, allow_conversion_ = false, follow_default_ = true;
    std::uint32_t gaps_ = 0, injected_gap_ = 0; std::uint64_t gap_frames_ = 0;
    bool registered_ = false, reattaching_ = false, detached_counted_ = false;
    EndpointRecovery recovery_{};
    audio::OutputGain* gain_ = nullptr;
    const audio::DspControls* gain_controls_ = nullptr;
    std::atomic<HRESULT> error_{S_OK};
    std::atomic<EndpointStatus> status_{EndpointStatus::Failed};
    std::atomic<std::uint64_t> silence_frames_{0};
};
}
