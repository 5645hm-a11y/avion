#include "media/platform/windows/Win32AacDecoder.hpp"
#include <wmcodecdsp.h>
#include <mferror.h>
#include <cstring>
#include <algorithm>
#include <bit>

namespace media::platform::windows {
namespace { template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } } }
Win32AacDecoder::~Win32AacDecoder() noexcept { Close(); }
bool Win32AacDecoder::Prepare(std::span<const std::byte> asc, unsigned rate, unsigned channels) noexcept {
    (void)channels;
    if (transform_ || asc.size() < 2 || asc.size() > 64) return false;
    if (codec::ParseAacConfig(asc, config_) != codec::AacConfigStatus::Ready || config_.sample_rate != rate || rate > 48000) {
        error_ = kUnsupportedLayout; return false;
    }
    if (config_.channels > kPlatformChannels) { error_ = kUnsupportedLayout; return false; } // 7.1: see the class comment.
    in_channels_ = config_.channels; stride_ = in_channels_ * 4;
    error_ = CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform_));
    IMFMediaType *input = nullptr, *output = nullptr;
    if (SUCCEEDED(error_)) error_ = MFCreateMediaType(&input);
    if (SUCCEEDED(error_)) error_ = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(error_)) error_ = input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED(error_)) error_ = input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(error_)) error_ = input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, in_channels_);
    if (SUCCEEDED(error_)) error_ = input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    // HEAACWAVEINFO tail (payload type 0 = raw, profile/level unknown), then the whole AudioSpecificConfig.
    BYTE config_bytes[12 + 64]{}; config_bytes[2] = 0xfe;
    std::memcpy(config_bytes + 12, asc.data(), asc.size());
    if (SUCCEEDED(error_)) error_ = input->SetBlob(MF_MT_USER_DATA, config_bytes, static_cast<UINT32>(12 + asc.size()));
    if (SUCCEEDED(error_)) error_ = transform_->SetInputType(0, input, 0);
    if (SUCCEEDED(error_)) {
        for (DWORD i = 0; i < 16 && !output; ++i) {
            IMFMediaType* available = nullptr;
            if (FAILED(transform_->GetOutputAvailableType(0, i, &available))) break;
            GUID subtype{}; UINT32 sr = 0, ch = 0, bits = 0, mask = 0;
            (void)available->GetGUID(MF_MT_SUBTYPE, &subtype);
            (void)available->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
            (void)available->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            (void)available->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            if (subtype == MFAudioFormat_Float && sr == rate && ch == in_channels_ && bits == 32) {
                // Mono/stereo types carry no mask: their layout is implied.
                decoder_mask_ = SUCCEEDED(available->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &mask)) && mask ? mask : config_.channel_mask;
                output = available;
            } else available->Release();
        }
        error_ = output ? transform_->SetOutputType(0, output, 0) : MF_E_INVALIDMEDIATYPE;
    }
    Release(input); Release(output); if (FAILED(error_)) return false;
    return Finish();
}
bool Win32AacDecoder::PrepareOpus(unsigned channels) noexcept {
    if (transform_ || (channels != 1 && channels != 2)) { error_ = kUnsupportedLayout; return false; }
    opus_ = true; in_channels_ = channels; stride_ = channels * 4;
    config_ = {}; config_.channels = channels; config_.sample_rate = 48000; config_.channel_mask = channels == 1 ? 0x4u : 0x3u;
    MFT_REGISTER_TYPE_INFO kind{MFMediaType_Audio, MFAudioFormat_Opus};
    IMFActivate** list = nullptr; UINT32 count = 0;
    error_ = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &kind, nullptr, &list, &count);
    if (SUCCEEDED(error_) && count) error_ = list[0]->ActivateObject(IID_PPV_ARGS(&transform_));
    else if (SUCCEEDED(error_)) error_ = kUnsupportedLayout;
    for (UINT32 i = 0; i < count; ++i) list[i]->Release();
    CoTaskMemFree(list);
    IMFMediaType *input = nullptr, *output = nullptr;
    if (SUCCEEDED(error_)) error_ = MFCreateMediaType(&input);
    if (SUCCEEDED(error_)) error_ = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(error_)) error_ = input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Opus);
    if (SUCCEEDED(error_)) error_ = input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    if (SUCCEEDED(error_)) error_ = input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(error_)) error_ = transform_->SetInputType(0, input, 0);
    for (DWORD i = 0; SUCCEEDED(error_) && i < 16 && !output; ++i) {
        IMFMediaType* available = nullptr;
        if (FAILED(transform_->GetOutputAvailableType(0, i, &available))) break;
        GUID subtype{}; UINT32 rate = 0, ch = 0;
        (void)available->GetGUID(MF_MT_SUBTYPE, &subtype); (void)available->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        (void)available->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        if (subtype == MFAudioFormat_Float && rate == 48000 && ch == channels) output = available; else available->Release();
    }
    if (SUCCEEDED(error_)) error_ = output ? transform_->SetOutputType(0, output, 0) : MF_E_INVALIDMEDIATYPE;
    Release(input); Release(output); if (FAILED(error_)) return false;
    decoder_mask_ = config_.channel_mask;
    return Finish();
}
bool Win32AacDecoder::Finish() noexcept {
    if (static_cast<unsigned>(std::popcount(decoder_mask_)) != in_channels_ || !codec::MapToOutput(decoder_mask_, map_)) { error_ = kUnsupportedLayout; return false; }
    out_channels_ = map_.out_channels; out_mask_ = map_.out_mask;
    MFT_INPUT_STREAM_INFO in{}; MFT_OUTPUT_STREAM_INFO out{};
    error_ = transform_->GetInputStreamInfo(0, &in);
    if (SUCCEEDED(error_)) error_ = transform_->GetOutputStreamInfo(0, &out);
    // AAC: one access unit's 1024 frames (at most 24 KiB); Opus: up to 120 ms, its transform asks for 512 KiB.
    if (FAILED(error_) || (out.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) || out.cbSize > (1u << 20) || in.cbSize > 65536) return false;
    output_capacity_ = (std::max)(65536u, static_cast<unsigned>(out.cbSize));
    for (auto& slot : inputs_) {
        error_ = MFCreateSample(&slot.storage);
        if (SUCCEEDED(error_)) error_ = MFCreateAlignedMemoryBuffer(65536, (std::max)(in.cbAlignment, DWORD{16}) - 1, &slot.buffer);
        if (SUCCEEDED(error_)) error_ = slot.AddBuffer(slot.buffer);
        if (SUCCEEDED(error_)) error_ = slot.SetSampleTime(0);
        if (SUCCEEDED(error_)) error_ = slot.SetSampleDuration(0);
        if (FAILED(error_)) return false;
    }
    error_ = MFCreateSample(&output_);
    if (SUCCEEDED(error_)) error_ = MFCreateAlignedMemoryBuffer(output_capacity_, (std::max)(out.cbAlignment, DWORD{16}) - 1, &output_buffer_);
    if (SUCCEEDED(error_)) error_ = output_->AddBuffer(output_buffer_);
    // Reserve timestamp/duration attributes before measuring streaming.
    if (SUCCEEDED(error_)) error_ = output_->SetSampleTime(0);
    if (SUCCEEDED(error_)) error_ = output_->SetSampleDuration(0);
    if (SUCCEEDED(error_)) error_ = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(error_)) error_ = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return SUCCEEDED(error_);
}
bool Win32AacDecoder::Decode(std::span<const std::byte> au, std::int64_t pts, std::int64_t duration) noexcept {
    if (!transform_ || Pending() || au.empty() || au.size() > 65536) return false;
    NativeSampleLease* input = nullptr;
    for (auto& slot : inputs_) if (slot.Available()) { input = &slot; break; }
    if (!input) { error_ = MF_E_NOTACCEPTING; return false; }
    BYTE* data = nullptr; error_ = input->buffer->Lock(&data, nullptr, nullptr);
    if (FAILED(error_)) return false;
    std::memcpy(data, au.data(), au.size()); (void)input->buffer->Unlock();
    error_ = input->buffer->SetCurrentLength(static_cast<DWORD>(au.size()));
    if (SUCCEEDED(error_)) error_ = input->SetSampleTime(pts);
    if (SUCCEEDED(error_)) error_ = input->SetSampleDuration(duration);
    if (SUCCEEDED(error_)) error_ = transform_->ProcessInput(0, input, 0);
    if (FAILED(error_)) return false;
    return Produce();
}
bool Win32AacDecoder::Drain() noexcept {
    if (!transform_ || Pending()) return false;
    if (!draining_) {
        error_ = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (SUCCEEDED(error_)) error_ = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(error_)) return false;
        draining_ = true;
    }
    return drained_ || Produce();
}
bool Win32AacDecoder::Produce() noexcept {
    bytes_ = offset_ = 0;
    (void)output_buffer_->SetCurrentLength(0);
    MFT_OUTPUT_DATA_BUFFER output{0, output_, 0, nullptr}; DWORD status = 0;
    error_ = transform_->ProcessOutput(0, 1, &output, &status);
    Release(output.pEvents);
    if (error_ == MF_E_TRANSFORM_NEED_MORE_INPUT) { drained_ = draining_; return true; }
    if (FAILED(error_)) return false; // Dynamic formats / delayed-output profiles are not admitted.
    DWORD bytes = 0; error_ = output_buffer_->GetCurrentLength(&bytes);
    // AAC: exactly one unit; Opus: whole frames, as many as the packet's TOC says.
    if (FAILED(error_) || (opus_ ? (!bytes || bytes % stride_ || bytes > output_capacity_) : bytes != 1024 * stride_)) return false;
    bytes_ = bytes; decoded_frames_ += bytes / stride_; return true;
}
bool Win32AacDecoder::Copy(std::byte* destination, unsigned frames) noexcept {
    BYTE* source = nullptr; error_ = output_buffer_->Lock(&source, nullptr, nullptr);
    if (FAILED(error_)) return false;
    if (map_.identity) std::memcpy(destination, source + offset_, static_cast<std::size_t>(frames) * stride_);
    else for (unsigned i = 0; i < frames; ++i) {
        float in[kPlatformChannels]{}, out[kPlatformChannels]{};
        std::memcpy(in, source + offset_ + static_cast<std::size_t>(i) * stride_, stride_);
        for (unsigned o = 0; o < out_channels_; ++o) for (unsigned c = 0; c < in_channels_; ++c) out[o] += map_.mix[o][c] * in[c];
        std::memcpy(destination + static_cast<std::size_t>(i) * out_channels_ * 4, out, out_channels_ * 4);
    }
    (void)output_buffer_->Unlock();
    // Linear fade-in over the armed ramp: a stream that starts mid-waveform does not step from silence.
    const unsigned channels = out_channels_;
    for (unsigned i = 0; i < frames && fade_done_ < fade_total_; ++i, ++fade_done_) {
        const float gain = static_cast<float>(fade_done_) / static_cast<float>(fade_total_);
        for (unsigned c = 0; c < channels; ++c) {
            float sample = 0; std::memcpy(&sample, destination + (static_cast<std::size_t>(i) * channels + c) * 4, 4);
            sample *= gain; std::memcpy(destination + (static_cast<std::size_t>(i) * channels + c) * 4, &sample, 4);
        }
    }
    return true;
}
EndpointStatus Win32AacDecoder::Transfer(Win32WasapiAudioSink& sink) noexcept {
    if (!Pending()) return EndpointStatus::Ready;
    if (sink.Format().channels != out_channels_) return EndpointStatus::InvalidArgument;
    AudioWriteSpan target{}; const auto state = sink.TryAcquire((bytes_ - offset_) / stride_, target);
    if (state != EndpointStatus::Ready) return state;
    if (!Copy(target.data, target.frames)) { (void)sink.Commit(target.lease, 0); return EndpointStatus::Failed; }
    const auto committed = sink.Commit(target.lease, target.frames);
    if (committed == EndpointStatus::Ready) offset_ += target.frames * stride_;
    return committed;
}
unsigned Win32AacDecoder::Read(std::span<std::byte> destination) noexcept {
    if (!Pending() || !stride_) return 0;
    const auto frames = (std::min)(static_cast<unsigned>(destination.size() / (out_channels_ * 4)), (bytes_ - offset_) / stride_);
    if (!frames || !Copy(destination.data(), frames)) return 0;
    offset_ += frames * stride_; return frames;
}
unsigned Win32AacDecoder::Skip(unsigned frames) noexcept {
    frames = (std::min)(frames, (bytes_ - offset_) / (stride_ ? stride_ : 1));
    offset_ += frames * stride_; return frames;
}
bool Win32AacDecoder::Flush() noexcept {
    if (!transform_) return false;
    // Drops queued input/output (including drain state) and starts a new stream on the same prepared MFT.
    error_ = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (SUCCEEDED(error_)) error_ = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    bytes_ = offset_ = 0; draining_ = drained_ = false; fade_done_ = fade_total_ = 0;
    return SUCCEEDED(error_);
}
void Win32AacDecoder::Close() noexcept {
    if (transform_) (void)transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    Release(transform_); Release(output_); Release(output_buffer_); opus_ = false;
    for (auto& input : inputs_) input.Close();
}
}
