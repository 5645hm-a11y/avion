#pragma once
// The platform's own software video decoder, a Media Foundation transform (2026-11-05, SDK API 17; ENGINE_MANIFESTO.md 2E): the mandatory
// fallback for a stream the GPU has no decoder for (AV1 on Intel UHD 630, Gen9.5). Hardware decoding is always tried first; this
// path is taken only when the GPU cannot take the stream. The transform is the operating system's (on Windows 11, the AV1 Video
// Extension); nothing is bundled.
//
// Synchronous: the decode owner feeds access units (Decode) and takes pictures (Next) in presentation order, each one's planes
// locked in system memory until Release; D3D12VideoDecoder::SubmitUpload copies them once into its surfaces. The input samples
// are made in Prepare (sixteen, of the stream's largest access unit); the output samples are the transform's own pool. The engine
// allocates nothing after Prepare.
#include "media/platform/windows/D3D12VideoDecoder.hpp"
#include "media/platform/windows/NativeSampleLease.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <span>

namespace media::platform::windows {
class Win32VideoMftDecoder final {
public:
    enum class Result : std::uint8_t { Ready, Full, NeedInput, Picture, Failed };
    Win32VideoMftDecoder() noexcept = default;
    ~Win32VideoMftDecoder() noexcept;
    Win32VideoMftDecoder(const Win32VideoMftDecoder&) = delete;
    Win32VideoMftDecoder& operator=(const Win32VideoMftDecoder&) = delete;
    // Whether this machine has a software decoder for `subtype` (MFVideoFormat_AV1); its name into `name` (UTF-16).
    [[nodiscard]] static bool Available(const GUID& subtype, wchar_t* name = nullptr, unsigned capacity = 0) noexcept;
    // `width` x `height`: the stream's picture; `ten_bit` asks for P010 output (else NV12). Media Foundation must be started.
    [[nodiscard]] bool Prepare(const GUID& subtype, unsigned width, unsigned height, bool ten_bit, std::uint32_t max_input_bytes) noexcept;
    // One access unit (temporal unit). Full: the transform holds pictures to take first (Next), then Decode again.
    [[nodiscard]] Result Decode(std::span<const std::byte> unit, std::int64_t pts_100ns, std::int64_t duration_100ns) noexcept;
    // The next decoded picture, its planes locked until Release (Picture); NeedInput when there is none yet (or, after Drain,
    // none left: Drained()).
    [[nodiscard]] Result Next(UploadPicture& picture) noexcept;
    void Release() noexcept;
    [[nodiscard]] bool Holding() const noexcept { return current_ != nullptr; }
    [[nodiscard]] bool Drain() noexcept;   // End of stream: the pictures it still holds come out of Next.
    [[nodiscard]] bool Drained() const noexcept { return drained_; }
    [[nodiscard]] bool Flush() noexcept;   // Seek: every picture and access unit it holds is dropped.
    void Close() noexcept;
    [[nodiscard]] HRESULT LastError() const noexcept { return error_; }
    [[nodiscard]] std::uint64_t Pictures() const noexcept { return pictures_; }
    [[nodiscard]] const wchar_t* Name() const noexcept { return name_; }

private:
    bool Output() noexcept; // Chooses the NV12/P010 output type and reads its frame size, stride and aperture.
    IMFTransform* transform_ = nullptr;
    NativeSampleLease inputs_[16]; // The transform keeps about ten before its first picture (frame threads).
    IMFSample* current_ = nullptr;
    IMFMediaBuffer* buffer_ = nullptr;
    IMF2DBuffer* plane_ = nullptr;
    GUID subtype_{};
    unsigned frame_width_ = 0, frame_height_ = 0, show_width_ = 0, show_height_ = 0, show_left_ = 0, show_top_ = 0;
    LONG stride_ = 0;
    bool ten_bit_ = false, draining_ = false, drained_ = false;
    std::uint64_t pictures_ = 0;
    HRESULT error_ = S_OK;
    wchar_t name_[64]{};
};
} // namespace media::platform::windows
