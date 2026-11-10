#pragma once
#include "media/audio/PcmRingBuffer.hpp"
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <span>

namespace media::platform::windows {
enum class CaptureSource : std::uint8_t {
    Microphone, // The default communications capture endpoint.
    Loopback,   // What the default render endpoint plays (tests, and "what was just said" on the programme itself).
    // What this process and its children render, and nothing else on the machine (2026-11-03; Windows 10 2004 and later:
    // AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK). A browser playing sound meanwhile is not heard. Prepare fails where
    // Windows has no process loopback.
    ProcessLoopback,
};
struct CaptureOptions {
    CaptureSource source = CaptureSource::Microphone;
    std::uint32_t sample_rate = 16000;  // Asked of Windows' shared-mode converter; SampleRate() is what was obtained.
    std::uint32_t ring_frames = 65536;  // Power of two: 4 s at 16 kHz, 1.4 s at 48 kHz.
};
// Owner-written, read by the owner or after it stopped.
struct CaptureStatistics {
    std::uint64_t frames = 0;        // Mono frames written to the ring.
    std::uint64_t dropped = 0;       // Frames discarded because the ring was full (the reader fell behind).
    std::uint32_t packets = 0, silent = 0, discontinuities = 0; // Endpoint packets; flagged silent; flagged glitches.
    std::int64_t first_qpc_100ns = 0; // Capture time of the first frame, on the monotonic clock the player uses.
};
// Shared-mode, event-driven WASAPI capture into a bounded lock-free ring of mono float32 PCM: the ingest end of the
// offline voice path (docs/voice.md). Nothing leaves the process and nothing is analysed here.
// - Prepare asks Windows for mono float32 at `sample_rate` (AUTOCONVERTPCM: the endpoint's own rate and channels are
//   converted by the audio engine). If the endpoint refuses, the stream runs in the endpoint's mix format and the
//   channels are averaged to mono here; the rate is then the endpoint's (SampleRate()).
// - Pump belongs to the capture owner and never waits: it polls the endpoint's event and moves every ready packet
//   into the ring. Event() may be waited on by an owner that has nothing else to do.
// - Read belongs to one other thread (the recogniser). The ring is single producer, single consumer.
// - A full ring drops the newest frames and counts them: capture never blocks and never allocates after Prepare.
// - An invalidated endpoint ends the stream (DeviceLost); the owner prepares a new one. There is no automatic
//   reattachment as in the render sink: a recogniser must see the gap.
class Win32WasapiCapture final {
public:
    Win32WasapiCapture() noexcept = default;
    ~Win32WasapiCapture() noexcept { Close(); }
    Win32WasapiCapture(const Win32WasapiCapture&) = delete;
    Win32WasapiCapture& operator=(const Win32WasapiCapture&) = delete;
    // Prepare, Start, Pump, Stop and Close belong to one COM-initialised owner thread.
    [[nodiscard]] EndpointStatus Prepare(core::LinearMemoryArena&, CaptureOptions = {}) noexcept;
    [[nodiscard]] EndpointStatus Start() noexcept;
    [[nodiscard]] EndpointStatus Pump() noexcept; // Ready: packets were moved; WouldBlock: none were due.
    [[nodiscard]] EndpointStatus Stop() noexcept;
    void Close() noexcept;
    // Reader thread: whole mono float32 frames, oldest first. Returns the count copied.
    [[nodiscard]] std::uint32_t Read(std::span<float> destination) noexcept;
    [[nodiscard]] std::uint32_t SampleRate() const noexcept { return sample_rate_; }
    [[nodiscard]] bool Converting() const noexcept { return converting_; }       // Windows delivers the asked format.
    [[nodiscard]] std::uint32_t EndpointChannels() const noexcept { return endpoint_channels_; }
    [[nodiscard]] std::uint32_t EndpointRate() const noexcept { return endpoint_rate_; }
    [[nodiscard]] HANDLE Event() const noexcept { return event_; }
    [[nodiscard]] HRESULT LastError() const noexcept { return error_; }
    [[nodiscard]] const CaptureStatistics& Stats() const noexcept { return stats_; }
private:
    [[nodiscard]] EndpointStatus Result(HRESULT) noexcept;
    void Write(const float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept; // Null: silence.
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    HANDLE event_ = nullptr;
    audio::PcmRingBuffer ring_;
    std::uint32_t sample_rate_ = 0, channels_ = 0, endpoint_channels_ = 0, endpoint_rate_ = 0;
    bool converting_ = false, running_ = false, lost_ = false;
    HRESULT error_ = S_OK;
    CaptureStatistics stats_{};
};
}
