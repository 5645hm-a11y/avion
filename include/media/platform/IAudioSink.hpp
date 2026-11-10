#pragma once
#include <cstddef>
#include <cstdint>

namespace media::platform {
enum class EndpointStatus : std::uint8_t { Ready, WouldBlock, InvalidArgument, DeviceLost, Failed };
enum class AudioEncoding : std::uint8_t { SignedPcm, Float32 };
struct AudioFormat {
    std::uint32_t sample_rate;
    std::uint16_t channels;
    std::uint16_t bytes_per_sample;
    AudioEncoding encoding = AudioEncoding::SignedPcm;
};
struct AudioWriteSpan { std::byte* data; std::uint32_t frames; std::uint64_t lease; };
struct AudioClockSample {
    std::uint64_t position;
    std::uint64_t frequency; // Position units per second; not assumed to be frames.
    std::int64_t monotonic_time_100ns; // Platform's normalized monotonic clock domain.
    bool precise;
    std::uint32_t generation = 0; // Changes on reset, device replacement, or a PCM continuity gap.
};
// Prepare native resources outside Running. Span ownership belongs to the sink
// until checkout and returns on Commit; no allocation/wait in these try methods.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    [[nodiscard]] virtual AudioFormat Format() const noexcept = 0;
    [[nodiscard]] virtual EndpointStatus TryAcquire(std::uint32_t frames, AudioWriteSpan&) noexcept = 0;
    [[nodiscard]] virtual EndpointStatus Commit(std::uint64_t lease, std::uint32_t frames) noexcept = 0;
    [[nodiscard]] virtual EndpointStatus Clock(AudioClockSample&) noexcept = 0;
};
}
