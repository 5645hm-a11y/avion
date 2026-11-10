#pragma once
#include "media/core/media_descriptors.hpp"
#include "media/core/spsc_ring_buffer.hpp"
#include "media/platform/IBlockSource.hpp"
#include "media/video/SurfaceLeasePool.hpp"
#include <Windows.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxva.h>
#include <span>

namespace media::platform::windows {
// Upload (2026-11-05, SDK API 17; ENGINE_MANIFESTO.md 2E): pictures a platform CPU decoder (a Media Foundation transform: AV1 on a GPU
// without AV1 decode) produced in system memory, copied once into the same NV12/P010 surfaces through a ring of kUploadSlots
// upload buffers on the copy queue; the copy signals the decode fence, so frames carry the same descriptors and leases as
// hardware decoding and everything after them is unchanged.
enum class VideoCodec : std::uint8_t { H264, Hevc, Upload };
enum class DecodeStatus : std::uint8_t { Submitted, Ready, WouldBlock, InvalidArgument, Unsupported, DeviceLost, Failed };
struct DecoderOptions {
    VideoCodec codec = VideoCodec::H264;
    std::uint16_t width = 1920, height = 1088;
    std::uint32_t surfaces = 20, max_bitstream_bytes = 4 * 1024 * 1024;
    std::uint32_t epoch = 1, decode_timeline_id = 1, consumer_timeline_id = 2;
    bool ten_bit = false;
};
struct NalSlice { std::uint32_t offset; std::uint32_t bytes; }; // Relative to MediaPacket's payload; excludes MP4 length prefix.
struct DecodePicture {
    core::MediaPacket packet{};
    BlockView payload{};
    std::span<const NalSlice> slices;
    std::span<const SurfaceHandle> references;
    // RefPic indices in these input records index references[], not pool IDs.
    // The backend remaps them and CurrPic to stable native DPB surface IDs.
    const DXVA_PicParams_H264* h264 = nullptr;
    const DXVA_Qmatrix_H264* h264_matrix = nullptr;
    const DXVA_PicParams_HEVC* hevc = nullptr;
    const DXVA_Qmatrix_HEVC* hevc_matrix = nullptr;
    std::uint32_t color_metadata_id = 0;
    std::uint16_t crop_right = 0, crop_bottom = 0; // Zero uses coded dimensions.
    std::uint16_t crop_left = 0, crop_top = 0;
    bool retain_as_reference = false;
    bool display = true; // False publishes the frame with VideoFrameFlags::NoDisplay.
};
// A decoded picture in system memory (VideoCodec::Upload): the luma plane and the interleaved chroma plane (NV12, or P010
// with ten_bit), `width` x `height` luma samples (at most the decoder's), rows `pitches` bytes apart.
struct UploadPicture {
    std::int64_t pts_100ns = 0, duration_100ns = 0;
    const std::byte* planes[2]{};
    std::uint32_t pitches[2]{};
    std::uint32_t width = 0, height = 0;
    std::uint32_t color_metadata_id = 0;
    std::uint16_t crop_left = 0, crop_top = 0, crop_right = 0, crop_bottom = 0; // Zero right/bottom: width/height.
    bool display = true;
};
struct VideoSurfaceReturn {
    std::uint32_t id, generation, epoch, timeline_id;
    std::uint64_t fence_value; // Zero means the consumer never submitted GPU reads.
};

// Complete native submission backend for PREPARED DXVA pictures. It does not
// synthesize picture parameters from SPS/PPS/slice syntax or implement codec MMCO/RPS.
// One decode owner; consumers return leases through SPSC, never concurrently here.
class D3D12VideoDecoder final {
public:
    D3D12VideoDecoder() noexcept = default;
    ~D3D12VideoDecoder() noexcept;
    D3D12VideoDecoder(const D3D12VideoDecoder&) = delete;
    D3D12VideoDecoder& operator=(const D3D12VideoDecoder&) = delete;
    [[nodiscard]] DecodeStatus Prepare(core::LinearMemoryArena&, ID3D12Device*, ID3D12Fence* consumer_fence,
                                       DecoderOptions = {}) noexcept;
    // Device-loss recovery, with every producer and consumer of this decoder quiescent. Suspend releases every
    // native object and device reference without waiting on the lost device's fences (its queues can no longer
    // retire anything). Resume rebuilds them on `device` with the same options and arena-backed state: every
    // lease is void, and frames of the rebuilt decoder carry `epoch` (the playback epoch continues). No arena use.
    void Suspend() noexcept;
    [[nodiscard]] DecodeStatus Resume(ID3D12Device*, ID3D12Fence* consumer_fence, std::uint32_t epoch) noexcept;
    [[nodiscard]] ID3D12Device* Device() const noexcept { return device_; } // Borrowed.
    template <std::size_t Frames, std::size_t Returns>
    [[nodiscard]] DecodeStatus Submit(const DecodePicture& picture,
        core::SpscRingBuffer<core::VideoFrameDescriptor, Frames>& output,
        core::SpscRingBuffer<core::MediaPacket, Returns>& payload_returns, SurfaceHandle* submitted = nullptr) noexcept {
        auto* frame = output.AcquireWriteSlot();
        if (!frame) return DecodeStatus::WouldBlock;
        auto* returned = payload_returns.AcquireWriteSlot();
        if (!returned) { (void)output.CancelWriteSlot(); return DecodeStatus::WouldBlock; }
        const auto status = SubmitImpl(picture, *frame);
        if (status != DecodeStatus::Submitted) {
            (void)output.CancelWriteSlot(); (void)payload_returns.CancelWriteSlot(); return status;
        }
        *returned = picture.packet; // CPU upload packing finished; source bytes no longer used.
        if (submitted) *submitted = {frame->surface_id, frame->surface_generation};
        (void)payload_returns.CommitWriteSlot(); (void)output.CommitWriteSlot(); return status;
    }
    // VideoCodec::Upload: copies `picture` into a free surface (CPU into an upload buffer, then the copy queue) and publishes
    // its frame. WouldBlock without a free surface or upload buffer; no allocation.
    template <std::size_t Frames>
    [[nodiscard]] DecodeStatus SubmitUpload(const UploadPicture& picture, core::SpscRingBuffer<core::VideoFrameDescriptor, Frames>& output,
                                            SurfaceHandle* submitted = nullptr) noexcept {
        auto* frame = output.AcquireWriteSlot();
        if (!frame) return DecodeStatus::WouldBlock;
        const auto status = UploadImpl(picture, *frame);
        if (status != DecodeStatus::Submitted) { (void)output.CancelWriteSlot(); return status; }
        if (submitted) *submitted = {frame->surface_id, frame->surface_generation};
        (void)output.CommitWriteSlot(); return status;
    }
    static constexpr std::uint32_t kUploadSlots = 4;
    [[nodiscard]] bool Uploading() const noexcept { return options_.codec == VideoCodec::Upload; }
    [[nodiscard]] bool ReturnSurface(VideoSurfaceReturn) noexcept;
    // Whether Submit would find a free surface now (no side effect; host mode's GPU decoding asks before parsing a picture).
    [[nodiscard]] bool CanReserve() const noexcept {
        if (!prepared_ || faulted_) return false;
        return leases_.CanReserve(decode_fence_->GetCompletedValue(), consumer_fence_ ? consumer_fence_->GetCompletedValue() : 0);
    }
    template <std::size_t Capacity>
    [[nodiscard]] DecodeStatus PollReturns(core::SpscRingBuffer<VideoSurfaceReturn, Capacity>& returns,
                                            std::uint32_t budget, std::uint32_t& processed) noexcept {
        processed = 0;
        while (processed < budget) {
            const auto* returned = returns.PeekReadSlot();
            if (!returned) break;
            const bool accepted = ReturnSurface(*returned);
            (void)returns.ReleaseReadSlot(); ++processed;
            if (!accepted) return DecodeStatus::InvalidArgument;
        }
        return DecodeStatus::Ready;
    }
    [[nodiscard]] bool ReleaseReference(SurfaceHandle handle) noexcept { return leases_.ReleaseReference(handle); }
    // Decode owner, on seek: later submissions must carry `epoch` and publish frames tagged with it. Consumer
    // returns of earlier epochs remain valid; surface generations, not epochs, prove lease identity.
    [[nodiscard]] bool SetEpoch(std::uint32_t epoch) noexcept { if (epoch <= epoch_) return false; epoch_ = epoch; return true; }
    [[nodiscard]] std::uint32_t Epoch() const noexcept { return epoch_; }
    // Decode-owner validation only (the lease ledger is not concurrent).
    [[nodiscard]] ID3D12Resource* Surface(SurfaceHandle handle) const noexcept;
    // Immutable table lookup for render/compute consumers holding a descriptor
    // lease. Resources stay alive until coordinated teardown. No AddRef per frame.
    [[nodiscard]] ID3D12Resource* SurfaceAt(std::uint32_t id) const noexcept {
        return contexts_ && id < options_.surfaces ? contexts_[id].surface : nullptr;
    }
    [[nodiscard]] DecodeStatus Statistics(SurfaceHandle, D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS&) noexcept;
    [[nodiscard]] ID3D12Fence* DecodeFence() const noexcept { return decode_fence_; } // Borrowed.
    [[nodiscard]] HRESULT LastError() const noexcept { return error_; }
    // Teardown only: WouldBlock until every published consumer lease is returned;
    // then waits for native queues and joined consumer timeline.
    [[nodiscard]] DecodeStatus DrainAndClose() noexcept;
    // Warm session (docs/session.md, "Warm switch"): the next file reuses this decoder when it is Compatible (the same
    // device, consumer fence, codec, size, bit depth, surfaces and bitstream capacity). Restart waits for the native
    // queues as DrainAndClose does (WouldBlock until every consumer lease is returned), then forgets every lease and
    // picture and starts again at `epoch`, keeping its native objects and its fences' values. No arena use.
    [[nodiscard]] bool Compatible(ID3D12Device*, ID3D12Fence* consumer_fence, const DecoderOptions&) const noexcept;
    [[nodiscard]] DecodeStatus Restart(std::uint32_t epoch) noexcept;
    // DrainAndClose, then forget the arena-backed state too, so that Prepare may run again (from a rewound arena).
    [[nodiscard]] DecodeStatus Close() noexcept;
    [[nodiscard]] bool Prepared() const noexcept { return prepared_; }
    // Fatal consumer failure: if queue retirement cannot be proven, retain native
    // objects until process exit instead of freeing memory a GPU may still access.
    void QuarantineGpuResources() noexcept { quarantined_ = true; faulted_ = true; }
private:
    static constexpr std::uint32_t MaxSlices = 256;
    struct Context {
        ID3D12Resource *surface = nullptr, *bitstream = nullptr, *upload = nullptr;
        ID3D12CommandAllocator *copy_allocator = nullptr, *decode_allocator = nullptr;
        ID3D12GraphicsCommandList* copy_list = nullptr;
        ID3D12VideoDecodeCommandList* decode_list = nullptr;
        std::byte* mapped = nullptr;
        std::uint64_t submitted_value = 0;
        DXVA_PicParams_H264 h264{};
        DXVA_Qmatrix_H264 h264_matrix{};
        DXVA_PicParams_HEVC hevc{};
        DXVA_Qmatrix_HEVC hevc_matrix{};
        DXVA_Slice_H264_Short h264_slices[MaxSlices]{};
        DXVA_Slice_HEVC_Short hevc_slices[MaxSlices]{};
    };
    [[nodiscard]] DecodeStatus SubmitImpl(const DecodePicture&, core::VideoFrameDescriptor&) noexcept;
    [[nodiscard]] DecodeStatus UploadImpl(const UploadPicture&, core::VideoFrameDescriptor&) noexcept;
    [[nodiscard]] DecodeStatus CreateUpload() noexcept;
    struct UploadSlot { ID3D12Resource* buffer = nullptr; std::byte* mapped = nullptr; std::uint64_t value = 0; };
    UploadSlot uploads_[kUploadSlots]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints_[2]{};
    UINT footprint_rows_[2]{};
    std::uint32_t next_upload_ = 0;
    [[nodiscard]] DecodeStatus CreateNative(ID3D12Device*, ID3D12Fence* consumer) noexcept;
    [[nodiscard]] DecodeStatus Result(HRESULT) noexcept;
    [[nodiscard]] bool Wait(ID3D12Fence*, std::uint64_t) noexcept;
    void ReleaseObjects() noexcept;
    Context* contexts_ = nullptr;
    video::SurfaceLeasePool leases_;
    DecoderOptions options_{};
    ID3D12Device* device_ = nullptr;
    ID3D12VideoDevice* video_ = nullptr;
    ID3D12VideoDecoder* decoder_ = nullptr;
    ID3D12VideoDecoderHeap* decoder_heap_ = nullptr;
    ID3D12Heap* bitstream_heap_ = nullptr;
    ID3D12CommandQueue *copy_queue_ = nullptr, *decode_queue_ = nullptr;
    ID3D12Fence *copy_fence_ = nullptr, *decode_fence_ = nullptr, *consumer_fence_ = nullptr;
    ID3D12QueryHeap* query_heap_ = nullptr;
    ID3D12Resource* statistics_buffer_ = nullptr;
    const std::byte* statistics_ = nullptr;
    HANDLE event_ = nullptr;
    std::uint64_t sequence_ = 0, consumer_high_water_ = 0;
    std::uint32_t epoch_ = 0;
    HRESULT error_ = S_OK;
    bool prepared_ = false, faulted_ = false, quarantined_ = false;
};
}
