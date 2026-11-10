#pragma once
// Hardware decoding in host mode (API 14; docs/sdk.md, "Hardware decoding in host mode"), Windows: the engine's own Direct3D 12
// video decoder (DXVA, the same as the windowed engine's) under host mode's HostEngine, its pictures converted on the GPU into
// texture output's shared textures (render::SharedOutput) for the host's renderer. Nothing of a picture comes back to the CPU.
//
//   HostEngine (the host's thread: parse, DPB, POC, clock) -> Decode: the DXVA picture on the decode queue, then (a picture to
//   show) one GPU copy into a picture ring of its own (kRing NV12/P010 surfaces), which frees the decoder's surface at once (a
//   picture waiting to be shown must not hold a reference the decoder needs: decode order is not presentation order)
//   -> Show (when the clock says): ColorPipeline converts the picture's view (zoom, pan) fitted into a free shared texture
//   (8-bit, or 10-bit PQ / FP16 scRGB, API 14), the frames fence signalled, the texture published.
//
// One direct queue and one fence (the decoder's consumer timeline): the decode queue waits on the GPU for a copy before decoding
// over its source, the direct queue for a decode before copying it; the host's release value is waited for on the GPU before a
// texture is drawn into again. The host's thread never waits for the GPU while playing (only a resize or close drains it).
#include "media/c_api.h"
#include "media/engine/HostEngine.hpp"
#include "media/platform/windows/D3D12VideoDecoder.hpp"
#include "media/platform/windows/H264Dxva.hpp"
#include "media/platform/windows/HevcDxva.hpp"
#include "media/render/ColorPipeline.hpp"
#include "media/render/SharedOutput.hpp"
#include "media/video/ColorDescription.hpp"
#include <atomic>
#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // The frame descriptors and rings are cache-line aligned on purpose.
#endif
namespace media::sdk {
class HostGpuVideo final : public engine::HostGpu {
public:
    static constexpr unsigned kRing = 8;      // Pictures decoded and waiting to be shown (a stream reordering more falls back).
    static constexpr unsigned kSurfaces = 16; // The decoder's: the DPB (at most the engine's 12 slots) and copies in flight.
    HostGpuVideo() noexcept = default;
    HostGpuVideo(const HostGpuVideo&) = delete;
    HostGpuVideo& operator=(const HostGpuVideo&) = delete;
    ~HostGpuVideo() noexcept { Release(); }
    // media_engine_open, before the stream is prepared: the texture output the host asked for.
    void Configure(unsigned width, unsigned height, render::SharedOutput::Format format) noexcept { width_ = width; height_ = height; format_ = format; }
    bool PrepareDecode(core::LinearMemoryArena& arena, const demux::TrackInfo& track, unsigned coded_width, unsigned coded_height, unsigned crop_height,
                       bool ten_bit, unsigned reorder) noexcept override;
    [[nodiscard]] bool Ready() const noexcept override;
    Status Decode(const codec::H264Picture* h264, const codec::HevcPicture* hevc, const core::MediaPacket& packet, const platform::BlockView& view,
                  bool display, const engine::HostGpuSlot* slots, unsigned slot_count, platform::SurfaceHandle& surface) noexcept override;
    void Unreference(platform::SurfaceHandle surface) noexcept override { if (prepared_) (void)decoder_.ReleaseReference(surface); }
    void Discard(platform::SurfaceHandle surface) noexcept override;
    bool Show(platform::SurfaceHandle surface, const engine::HostGpuShow& show) noexcept override;
    bool Restart(std::uint32_t epoch) noexcept override;
    void CloseDecode() noexcept override;
    void Release() noexcept; // Everything, the device too (media_engine_destroy).
    // The SDK's texture calls (docs/sdk.md, "Texture output"), on one host thread: valid while a stream is prepared.
    [[nodiscard]] bool Active() const noexcept { return prepared_; }
    [[nodiscard]] render::SharedOutput& Output() noexcept { return output_; }
    [[nodiscard]] render::SharedOutput::Format Format() const noexcept { return format_; }
    void RequestSize(unsigned width, unsigned height) noexcept { size_request_.store(std::uint64_t{width} << 32 | height, std::memory_order_release); }
    struct Stats { std::uint32_t decoded = 0, copied = 0, shown = 0, discarded = 0, no_texture = 0, failed = 0; };
    [[nodiscard]] const Stats& Statistics() const noexcept { return stats_; }
private:
    struct Picture {
        ID3D12Resource* texture = nullptr;     // NV12 / P010, the decoder's surface description.
        ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr; // Its copy.
        std::uint64_t fence = 0;               // The last GPU work reading or writing it (copy, conversion).
        platform::SurfaceHandle owner{};       // The decoder's picture it holds.
        core::VideoFrameDescriptor frame{};
        bool waiting = false;                  // Holds a picture to show.
    };
    struct Target { ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr; std::uint64_t fence = 0; };
    bool CreateDevice() noexcept;
    bool CreateTargets(unsigned width, unsigned height) noexcept; // The shared textures and their UAVs.
    bool Idle(unsigned milliseconds) noexcept;                    // The direct queue's work done (resize, close).
    [[nodiscard]] int FreePicture() const noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned i) const noexcept;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Gpu(unsigned i) const noexcept;
    void ApplySize() noexcept;

    unsigned width_ = 0, height_ = 0;
    render::SharedOutput::Format format_ = render::SharedOutput::Format::Rgba8;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ID3D12Fence* fence_ = nullptr;            // The direct queue's timeline: the decoder's consumer fence.
    ID3D12DescriptorHeap* heap_ = nullptr;    // kRing x 2 plane SRVs, then the three textures' UAVs.
    HANDLE event_ = nullptr;
    unsigned stride_ = 0;
    std::uint64_t value_ = 0;
    render::ColorPipeline color_;
    render::SharedOutput output_;
    render::PeakTiming peak_;
    platform::windows::D3D12VideoDecoder decoder_;
    codec::H264Dxva h264_dxva_{};
    codec::HevcDxva hevc_dxva_{};
    core::SpscRingBuffer<core::VideoFrameDescriptor, 4> frames_;
    core::SpscRingBuffer<core::MediaPacket, 4> returns_;
    Picture ring_[kRing]{};
    Target targets_[render::SharedOutput::kCount]{};
    video::ColorDescription container_{};
    unsigned crop_height_ = 0;
    bool prepared_ = false, ten_bit_ = false;
    std::atomic<std::uint64_t> size_request_{0};
    Stats stats_{};
};

// Texture output's C structs from SharedOutput (the windowed engine's and host mode's): the API 13 struct (72, 32 bytes) or
// API 14's (80, 40: the colour space, the SDR white, the picture's transfer), as the host's struct_size says.
void FillTextures(const render::SharedOutput::Desc& d, render::SharedOutput::Format format, media_output_textures_t* out) noexcept;
void FillFrame(const render::SharedOutput::Frame& f, media_output_frame_t* out) noexcept;
[[nodiscard]] render::SharedOutput::Format OutputFormat(std::uint32_t media_output_format) noexcept;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
