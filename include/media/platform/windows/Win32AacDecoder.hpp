#pragma once
#include "media/platform/windows/Win32WasapiAudioSink.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <span>
#include "media/platform/windows/NativeSampleLease.hpp"
#include "media/codec/AacConfig.hpp"
namespace media::platform::windows {
// Native synchronous AAC-LC MFT. All samples and buffers are created in Prepare.
// Channel layouts: the AudioSpecificConfig (including a PCE) is parsed and passed whole to the MFT, which outputs
// float PCM in WAVEFORMATEXTENSIBLE order with its channel mask. Output is mono, stereo, or 5.1 (FL FR FC LFE BL BR):
// 3-, 4- and 5-channel programmes are placed on the 5.1 layout (missing speakers silent, a back centre split -3 dB
// to both surrounds, side surrounds on the surround pair). The Microsoft decoder accepts at most 6 channels (5.1);
// 7.1 is refused at every entry point on Windows 11 25H2, so Prepare reports kUnsupportedLayout for it.
class Win32AacDecoder final {
public:
    static constexpr unsigned kPlatformChannels = 6;
    static constexpr HRESULT kUnsupportedLayout = static_cast<HRESULT>(0xA0000AACu); // Customer-defined failure: layout not decodable.
    ~Win32AacDecoder() noexcept;
    // `channels` is the sample entry's count; the ASC decides (muxers disagree on what PCE streams declare).
    bool Prepare(std::span<const std::byte> asc, unsigned sample_rate, unsigned channels) noexcept;
    // Opus (API 17, Matroska/WebM): the platform's Opus decoder (a Media Foundation transform, no configuration needed),
    // 48 kHz float, mono or stereo; packets of any duration the TOC allows (2.5 to 120 ms).
    bool PrepareOpus(unsigned channels) noexcept;
    bool Opus() const noexcept { return opus_; }
    unsigned Channels() const noexcept { return out_channels_; }             // Output PCM channels (1, 2 or 6).
    std::uint32_t OutputMask() const noexcept { return out_mask_; }          // Speakers of the output layout.
    std::uint32_t DecoderMask() const noexcept { return decoder_mask_; }     // Speakers the MFT reported.
    const codec::AacConfig& Config() const noexcept { return config_; }
    bool Decode(std::span<const std::byte> au, std::int64_t pts, std::int64_t duration) noexcept;
    bool Drain() noexcept;
    bool Flush() noexcept; // Seek: discard pending PCM and codec state, including a completed drain.
    bool Drained() const noexcept { return drained_; }
    unsigned DecodedFrames() const noexcept { return decoded_frames_; }
    EndpointStatus Transfer(Win32WasapiAudioSink&) noexcept;
    // Copies pending PCM (float32 interleaved) into memory instead of an endpoint; returns whole frames copied.
    unsigned Read(std::span<std::byte> destination) noexcept;
    // Seek pre-roll: drops up to `frames` pending PCM frames that precede the target. Returns frames dropped.
    unsigned Skip(unsigned frames) noexcept;
    // Arms a linear gain ramp over the next `frames` delivered frames (0 disarms).
    void FadeIn(unsigned frames) noexcept { fade_total_ = frames; fade_done_ = 0; }
    unsigned PendingFrames() const noexcept { return stride_ ? (bytes_ - offset_) / stride_ : 0; }
    bool Pending() const noexcept { return offset_ < bytes_; }
    HRESULT LastError() const noexcept { return error_; }
    void Close() noexcept;
private:
    bool Produce() noexcept;
    bool Copy(std::byte* destination, unsigned frames) noexcept;
    IMFTransform* transform_ = nullptr;
    NativeSampleLease inputs_[8];
    IMFSample* output_ = nullptr;
    IMFMediaBuffer* output_buffer_ = nullptr;
    unsigned stride_ = 0, bytes_ = 0, offset_ = 0;           // stride_: decoder output bytes per frame.
    unsigned in_channels_ = 0, out_channels_ = 0;
    std::uint32_t decoder_mask_ = 0, out_mask_ = 0;
    codec::ChannelMap map_{};                               // Decoder layout -> output layout gains.
    codec::AacConfig config_{};
    unsigned decoded_frames_ = 0, fade_done_ = 0, fade_total_ = 0;
    bool draining_ = false, drained_ = false, opus_ = false;
    unsigned output_capacity_ = 0;
    bool Finish() noexcept; // Output type chosen: streams, samples and buffers made.
    HRESULT error_ = S_OK;
};
}
