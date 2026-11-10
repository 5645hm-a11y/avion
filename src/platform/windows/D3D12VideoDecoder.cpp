#include "media/platform/windows/D3D12VideoDecoder.hpp"
#include <algorithm>
#include <cstring>
#include <memory>

namespace media::platform::windows {
namespace {
template <typename T> void Release(T*& object) noexcept { if (object) { object->Release(); object = nullptr; } }
D3D12_RESOURCE_DESC Buffer(std::uint64_t bytes) noexcept {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return desc;
}
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to}; return barrier;
}
}
D3D12VideoDecoder::~D3D12VideoDecoder() noexcept { (void)DrainAndClose(); }
DecodeStatus D3D12VideoDecoder::Result(HRESULT hr) noexcept {
    error_ = hr;
    if (SUCCEEDED(hr)) return DecodeStatus::Ready;
    faulted_ = true;
    return device_ && FAILED(device_->GetDeviceRemovedReason()) ? DecodeStatus::DeviceLost : DecodeStatus::Failed;
}
DecodeStatus D3D12VideoDecoder::Prepare(core::LinearMemoryArena& arena, ID3D12Device* device,
                                       ID3D12Fence* consumer, DecoderOptions options) noexcept {
    const bool upload = options.codec == VideoCodec::Upload;
    if (device_ || contexts_ || !device || arena.IsSealed() || options.surfaces < 2 || options.surfaces > 32 ||
        !options.width || !options.height || options.width % (upload ? 2 : 8) || options.height % (upload ? 2 : 8) ||
        (options.codec != VideoCodec::H264 && options.codec != VideoCodec::Hevc && !upload) ||
        (options.codec == VideoCodec::H264 && (options.ten_bit || options.width % 16 || options.height % 16)) ||
        options.max_bitstream_bytes < 128 || options.max_bitstream_bytes > (1u << 28) ||
        options.max_bitstream_bytes % 128 || options.decode_timeline_id == options.consumer_timeline_id)
        return DecodeStatus::InvalidArgument;
    options_ = options;
    // Host memory once, from the arena: the lease ledger and the per-surface submission contexts. A device-loss
    // recovery (Resume) reuses both.
    if (!leases_.Initialize(arena, options.surfaces)) { error_ = E_OUTOFMEMORY; return DecodeStatus::Failed; }
    contexts_ = static_cast<Context*>(arena.Allocate(sizeof(Context) * options.surfaces, alignof(Context)));
    if (!contexts_) { error_ = E_OUTOFMEMORY; return DecodeStatus::Failed; }
    for (std::uint32_t i = 0; i < options.surfaces; ++i) std::construct_at(contexts_ + i);
    return CreateNative(device, consumer);
}
void D3D12VideoDecoder::Suspend() noexcept {
    // The device is gone (or going): nothing it executes can be waited for, and nothing is recycled. Every native
    // object and every device reference is released; the arena-backed ledger and contexts stay for Resume.
    if (!quarantined_) ReleaseObjects();
}
DecodeStatus D3D12VideoDecoder::Resume(ID3D12Device* device, ID3D12Fence* consumer, std::uint32_t epoch) noexcept {
    if (quarantined_ || !contexts_ || !device || epoch < options_.epoch) return DecodeStatus::InvalidArgument;
    ReleaseObjects();
    leases_.Reset();
    for (std::uint32_t i = 0; i < options_.surfaces; ++i) { std::destroy_at(contexts_ + i); std::construct_at(contexts_ + i); }
    // Returns of earlier epochs belong to the lost device: the rebuilt ledger starts at the recovery epoch.
    options_.epoch = epoch; sequence_ = consumer_high_water_ = 0; faulted_ = false;
    return CreateNative(device, consumer);
}
DecodeStatus D3D12VideoDecoder::CreateNative(ID3D12Device* device, ID3D12Fence* consumer) noexcept {
    const auto& options = options_;
    device_ = device; device_->AddRef();
    consumer_fence_ = consumer; if (consumer) consumer->AddRef();
    auto fail = [&](HRESULT hr, DecodeStatus state = DecodeStatus::Failed) noexcept {
        error_ = hr; ReleaseObjects(); return state;
    };
    if (options.codec == VideoCodec::Upload) return CreateUpload();
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&video_));
    if (FAILED(hr)) return fail(hr, DecodeStatus::Unsupported);
    D3D12_VIDEO_DECODE_CONFIGURATION configuration{};
    configuration.DecodeProfile = options.codec == VideoCodec::H264 ? D3D12_VIDEO_DECODE_PROFILE_H264 :
        options.ten_bit ? D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10 : D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
    configuration.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
    configuration.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
    const auto format = options.ten_bit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support{};
    support.Configuration = configuration; support.Width = options.width; support.Height = options.height;
    support.DecodeFormat = format; support.FrameRate = {60, 1}; support.BitRate = 0;
    hr = video_->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &support, sizeof(support));
    if (FAILED(hr) || !(support.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED) ||
        support.DecodeTier < D3D12_VIDEO_DECODE_TIER_2 ||
        (support.ConfigurationFlags & D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_REFERENCE_ONLY_ALLOCATIONS_REQUIRED))
        return fail(FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED, DecodeStatus::Unsupported);
    for (auto plane : {options.ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
                       options.ten_bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM}) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT feature{plane, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        hr = device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &feature, sizeof(feature));
        if (FAILED(hr) || !(feature.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
            return fail(FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED, DecodeStatus::Unsupported);
    }
    D3D12_VIDEO_DECODER_DESC decoder_desc{0, configuration};
    hr = video_->CreateVideoDecoder(&decoder_desc, IID_PPV_ARGS(&decoder_));
    D3D12_VIDEO_DECODER_HEAP_DESC heap_desc{0, configuration, options.width, options.height, format,
                                         {60, 1}, 0, options.surfaces};
    if (SUCCEEDED(hr)) hr = video_->CreateVideoDecoderHeap(&heap_desc, IID_PPV_ARGS(&decoder_heap_));
    if (FAILED(hr)) return fail(hr);
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = options.width;
    texture.Height = (support.ConfigurationFlags & D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_HEIGHT_ALIGNMENT_MULTIPLE_32_REQUIRED) ?
        (static_cast<UINT>(options.height) + 31u) & ~31u : options.height;
    texture.DepthOrArraySize = 1; texture.MipLevels = 1; texture.Format = format; texture.SampleDesc.Count = 1;
    const auto bitstream = Buffer(options.max_bitstream_bytes);
    const auto texture_info = device_->GetResourceAllocationInfo(0, 1, &texture);
    const auto buffer_info = device_->GetResourceAllocationInfo(0, 1, &bitstream);
    if (texture_info.SizeInBytes == UINT64_MAX || buffer_info.SizeInBytes == UINT64_MAX ||
        texture_info.SizeInBytes > UINT64_MAX / options.surfaces || buffer_info.SizeInBytes > UINT64_MAX / options.surfaces)
        return fail(E_INVALIDARG);
    D3D12_HEAP_DESC memory{};
    memory.SizeInBytes = buffer_info.SizeInBytes * options.surfaces;
    memory.Alignment = buffer_info.Alignment; memory.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    memory.Properties.CreationNodeMask = memory.Properties.VisibleNodeMask = 1;
    memory.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    hr = device_->CreateHeap(&memory, IID_PPV_ARGS(&bitstream_heap_));
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (SUCCEEDED(hr)) hr = device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&copy_queue_));
    queue.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
    if (SUCCEEDED(hr)) hr = device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&decode_queue_));
    if (SUCCEEDED(hr)) hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence_));
    if (SUCCEEDED(hr)) hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&decode_fence_));
    if (FAILED(hr)) return fail(hr);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return fail(HRESULT_FROM_WIN32(GetLastError()));
    D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_VIDEO_DECODE_STATISTICS, options.surfaces, 0};
    hr = device_->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query_heap_));
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_READBACK; properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    const auto statistics_desc = Buffer(sizeof(D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS) * options.surfaces);
    if (SUCCEEDED(hr)) hr = device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &statistics_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&statistics_buffer_));
    void* mapping = nullptr;
    if (SUCCEEDED(hr)) hr = statistics_buffer_->Map(0, nullptr, &mapping);
    if (FAILED(hr)) return fail(hr);
    statistics_ = static_cast<const std::byte*>(mapping);
    // Decode surfaces are committed resources. Textures placed together in one explicit heap were
    // written correctly but read back as zeros when used as motion-compensation references by the
    // Intel UHD 630 media engine (driver-managed compression metadata); committed allocations let the
    // driver attach that per-resource state. They are created here, before sealing, and again only on the
    // replacement device after a device loss.
    D3D12_HEAP_PROPERTIES local{};
    local.Type = D3D12_HEAP_TYPE_DEFAULT; local.CreationNodeMask = local.VisibleNodeMask = 1;
    properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    for (std::uint32_t i = 0; i < options.surfaces; ++i) {
        auto& context = contexts_[i];
        hr = device_->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&context.surface));
        if (SUCCEEDED(hr)) hr = device_->CreatePlacedResource(bitstream_heap_, i * buffer_info.SizeInBytes, &bitstream,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&context.bitstream));
        if (SUCCEEDED(hr)) hr = device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bitstream,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&context.upload));
        const D3D12_RANGE no_read{0, 0}; mapping = nullptr;
        if (SUCCEEDED(hr)) hr = context.upload->Map(0, &no_read, &mapping);
        context.mapped = static_cast<std::byte*>(mapping);
        if (SUCCEEDED(hr)) hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&context.copy_allocator));
        if (SUCCEEDED(hr)) hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, IID_PPV_ARGS(&context.decode_allocator));
        if (SUCCEEDED(hr)) hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, context.copy_allocator,
            nullptr, IID_PPV_ARGS(&context.copy_list));
        if (SUCCEEDED(hr)) hr = context.copy_list->Close();
        if (SUCCEEDED(hr)) hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, context.decode_allocator,
            nullptr, IID_PPV_ARGS(&context.decode_list));
        if (SUCCEEDED(hr)) hr = context.decode_list->Close();
        if (FAILED(hr)) return fail(hr);
    }
    epoch_ = options_.epoch; prepared_ = true; return DecodeStatus::Ready;
}

DecodeStatus D3D12VideoDecoder::SubmitImpl(const DecodePicture& picture, core::VideoFrameDescriptor& frame) noexcept {
    if (!prepared_ || faulted_) return DecodeStatus::Failed;
    const auto& packet = picture.packet;
    if (packet.playback_epoch != epoch_ || !picture.payload.data || packet.pts_100ns == INT64_MIN ||
        packet.duration_100ns < 0 || packet.payload_block_id != picture.payload.token.id ||
        packet.payload_generation != picture.payload.token.generation || packet.payload_offset > picture.payload.valid_bytes ||
        packet.payload_bytes > picture.payload.valid_bytes - packet.payload_offset || picture.slices.empty() ||
        picture.slices.size() > MaxSlices || picture.references.size() > (options_.codec == VideoCodec::H264 ? 16u : 15u) ||
        picture.crop_right > options_.width || picture.crop_bottom > options_.height ||
        picture.crop_left >= (picture.crop_right ? picture.crop_right : options_.width) ||
        picture.crop_top >= (picture.crop_bottom ? picture.crop_bottom : options_.height) || sequence_ >= UINT64_MAX - 2)
        return DecodeStatus::InvalidArgument;
    const bool h264 = options_.codec == VideoCodec::H264;
    if (h264) {
        if (!picture.h264 || !picture.h264_matrix) return DecodeStatus::InvalidArgument;
        const auto& p = *picture.h264;
        if ((static_cast<UINT>(p.wFrameWidthInMbsMinus1) + 1) * 16 != options_.width ||
            (static_cast<UINT>(p.wFrameHeightInMbsMinus1) + 1) * 16 != options_.height ||
            !p.frame_mbs_only_flag || p.field_pic_flag || p.MbaffFrameFlag || p.chroma_format_idc != 1 ||
            p.bit_depth_luma_minus8 || p.bit_depth_chroma_minus8 || p.num_slice_groups_minus1)
            return DecodeStatus::Unsupported;
        for (const auto ref : p.RefFrameList)
            if (ref.bPicEntry != 255 && ref.Index7Bits >= picture.references.size()) return DecodeStatus::InvalidArgument;
    } else {
        if (!picture.hevc || !picture.hevc_matrix) return DecodeStatus::InvalidArgument;
        const auto& p = *picture.hevc;
        if (p.log2_min_luma_coding_block_size_minus3 > 3 || p.chroma_format_idc != 1 || p.separate_colour_plane_flag ||
            p.bit_depth_luma_minus8 != (options_.ten_bit ? 2u : 0u) || p.bit_depth_chroma_minus8 != p.bit_depth_luma_minus8)
            return DecodeStatus::Unsupported;
        const UINT block = 1u << (p.log2_min_luma_coding_block_size_minus3 + 3);
        if (p.PicWidthInMinCbsY * block != options_.width || p.PicHeightInMinCbsY * block != options_.height)
            return DecodeStatus::InvalidArgument;
        for (const auto ref : p.RefPicList)
            if (ref.bPicEntry != 255 && ref.Index7Bits >= picture.references.size()) return DecodeStatus::InvalidArgument;
    }
    const auto* payload = picture.payload.data + packet.payload_offset;
    std::uint64_t packed_bytes = 0, wait_value = 0;
    for (const auto slice : picture.slices) {
        if (slice.offset > packet.payload_bytes || slice.bytes > packet.payload_bytes - slice.offset ||
            slice.bytes < (h264 ? 1u : 2u)) return DecodeStatus::InvalidArgument;
        const auto header = std::to_integer<unsigned>(payload[slice.offset]);
        if ((header & 128) || (h264 && (header & 31) != 1 && (header & 31) != 5) ||
            (!h264 && (((header >> 1) & 63) > 31 || !(std::to_integer<unsigned>(payload[slice.offset + 1]) & 7))))
            return DecodeStatus::InvalidArgument;
        packed_bytes += 3ull + slice.bytes;
        if (packed_bytes > options_.max_bitstream_bytes) return DecodeStatus::InvalidArgument;
    }
    const auto padded = (packed_bytes + 127) & ~std::uint64_t{127};
    if (padded > options_.max_bitstream_bytes) return DecodeStatus::InvalidArgument;
    for (std::size_t i = 0; i < picture.references.size(); ++i) {
        const auto reference = picture.references[i];
        std::uint64_t value = 0;
        if (!leases_.ReferenceReady(reference, value)) return DecodeStatus::WouldBlock;
        for (std::size_t j = 0; j < i; ++j)
            if (picture.references[j].id == reference.id) return DecodeStatus::InvalidArgument;
        wait_value = (std::max)(wait_value, value);
    }
    const auto decoded = decode_fence_->GetCompletedValue();
    const auto consumed = consumer_fence_ ? consumer_fence_->GetCompletedValue() : 0;
    if (decoded == UINT64_MAX || consumed == UINT64_MAX) return Result(DXGI_ERROR_DEVICE_REMOVED);
    SurfaceHandle handle{};
    if (!leases_.Reserve(decoded, consumed, handle)) return DecodeStatus::WouldBlock;
    auto& context = contexts_[handle.id];
    UINT written = 0, slice_index = 0;
    for (const auto slice : picture.slices) {
        context.h264_slices[slice_index] = {written, slice.bytes + 3, 0};
        context.hevc_slices[slice_index] = {written, slice.bytes + 3, 0};
        context.mapped[written] = context.mapped[written + 1] = std::byte{0}; context.mapped[written + 2] = std::byte{1};
        std::memcpy(context.mapped + written + 3, payload + slice.offset, slice.bytes);
        written += slice.bytes + 3; ++slice_index;
    }
    std::memset(context.mapped + written, 0, static_cast<std::size_t>(padded - written));
    const auto pad = static_cast<UINT>(padded - written);
    context.h264_slices[slice_index - 1].SliceBytesInBuffer += pad;
    context.hevc_slices[slice_index - 1].SliceBytesInBuffer += pad;
    if (h264) {
        context.h264 = *picture.h264; context.h264_matrix = *picture.h264_matrix;
        context.h264.CurrPic.bPicEntry = static_cast<UCHAR>(handle.id);
        context.h264.StatusReportFeedbackNumber = static_cast<UINT>(sequence_ % UINT32_MAX) + 1u;
        for (auto& ref : context.h264.RefFrameList)
            if (ref.bPicEntry != 255) ref.bPicEntry = static_cast<UCHAR>((ref.bPicEntry & 128u) | picture.references[ref.Index7Bits].id);
    } else {
        context.hevc = *picture.hevc; context.hevc_matrix = *picture.hevc_matrix;
        context.hevc.CurrPic.bPicEntry = static_cast<UCHAR>(handle.id);
        context.hevc.StatusReportFeedbackNumber = static_cast<UINT>(sequence_ % UINT32_MAX) + 1u;
        for (auto& ref : context.hevc.RefPicList)
            if (ref.bPicEntry != 255) ref.bPicEntry = static_cast<UCHAR>((ref.bPicEntry & 128u) | picture.references[ref.Index7Bits].id);
    }
    HRESULT hr = context.copy_allocator->Reset();
    if (SUCCEEDED(hr)) hr = context.copy_list->Reset(context.copy_allocator, nullptr);
    if (SUCCEEDED(hr)) hr = context.decode_allocator->Reset();
    if (SUCCEEDED(hr)) hr = context.decode_list->Reset(context.decode_allocator);
    if (FAILED(hr)) { (void)leases_.Cancel(handle); return Result(hr); }
    auto barrier = Transition(context.bitstream, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    context.copy_list->ResourceBarrier(1, &barrier);
    context.copy_list->CopyBufferRegion(context.bitstream, 0, context.upload, 0, padded);
    barrier = Transition(context.bitstream, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    context.copy_list->ResourceBarrier(1, &barrier);
    hr = context.copy_list->Close();
    if (FAILED(hr)) { (void)leases_.Cancel(handle); return Result(hr); }
    D3D12_RESOURCE_BARRIER barriers[18]{};
    UINT barrier_count = 0;
    barriers[barrier_count++] = Transition(context.bitstream, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_DECODE_READ);
    barriers[barrier_count++] = Transition(context.surface, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE);
    for (auto ref : picture.references)
        barriers[barrier_count++] = Transition(contexts_[ref.id].surface, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_DECODE_READ);
    context.decode_list->ResourceBarrier(barrier_count, barriers);
    ID3D12Resource* reference_resources[32]{};
    UINT subresources[32]{};
    if (!picture.references.empty()) {
        // Stable DPB indices; unused entries alias the first valid read-state
        // resource. No null or unrelated, untransitioned texture is passed.
        for (UINT i = 0; i < options_.surfaces; ++i) reference_resources[i] = contexts_[picture.references[0].id].surface;
        for (auto ref : picture.references) reference_resources[ref.id] = contexts_[ref.id].surface;
        // CurrPic indexes this same array: drivers keep per-picture side data (e.g. HEVC collocated
        // motion vectors) in that DPB slot, so it must name the output, never an aliased reference.
        reference_resources[handle.id] = context.surface;
    }
    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS input{};
    UINT arguments = 0;
    input.FrameArguments[arguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS,
        h264 ? static_cast<UINT>(sizeof(context.h264)) : static_cast<UINT>(sizeof(context.hevc)),
        h264 ? static_cast<void*>(&context.h264) : static_cast<void*>(&context.hevc)};
    // HEVC carries inverse-quantization matrices only when scaling_list_enabled_flag is set.
    if (h264 || context.hevc.scaling_list_enabled_flag)
        input.FrameArguments[arguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX,
            h264 ? static_cast<UINT>(sizeof(context.h264_matrix)) : static_cast<UINT>(sizeof(context.hevc_matrix)),
            h264 ? static_cast<void*>(&context.h264_matrix) : static_cast<void*>(&context.hevc_matrix)};
    input.FrameArguments[arguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL,
        slice_index * static_cast<UINT>(h264 ? sizeof(DXVA_Slice_H264_Short) : sizeof(DXVA_Slice_HEVC_Short)),
        h264 ? static_cast<void*>(context.h264_slices) : static_cast<void*>(context.hevc_slices)};
    input.NumFrameArguments = arguments;
    input.ReferenceFrames = {picture.references.empty() ? 0u : options_.surfaces, reference_resources, subresources, nullptr};
    input.CompressedBitstream = {context.bitstream, 0, padded}; input.pHeap = decoder_heap_;
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS output{}; output.pOutputTexture2D = context.surface;
    context.decode_list->DecodeFrame(decoder_, &output, &input);
    context.decode_list->EndQuery(query_heap_, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, handle.id);
    context.decode_list->ResolveQueryData(query_heap_, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, handle.id, 1,
        statistics_buffer_, handle.id * sizeof(D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS));
    for (UINT i = 0; i < barrier_count; ++i) std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
    context.decode_list->ResourceBarrier(barrier_count, barriers);
    hr = context.decode_list->Close();
    if (FAILED(hr)) { (void)leases_.Cancel(handle); return Result(hr); }
    const auto value = ++sequence_;
    ID3D12CommandList* copies[]{context.copy_list}; copy_queue_->ExecuteCommandLists(1, copies);
    hr = copy_queue_->Signal(copy_fence_, value);
    if (SUCCEEDED(hr)) hr = decode_queue_->Wait(copy_fence_, value);
    if (SUCCEEDED(hr) && wait_value) hr = decode_queue_->Wait(consumer_fence_, wait_value);
    if (FAILED(hr)) return Result(hr); // Quarantined; never recycle a partially submitted context.
    ID3D12CommandList* decodes[]{context.decode_list}; decode_queue_->ExecuteCommandLists(1, decodes);
    hr = decode_queue_->Signal(decode_fence_, value);
    if (FAILED(hr)) return Result(hr);
    // A picture that is not displayed is never read by the presentation side: as a reference it needs no return first,
    // so a seek's chain of reference-only pictures is submitted back to back (docs/seeking.md, "Seek catch-up").
    (void)leases_.Submitted(handle, value, picture.retain_as_reference, !picture.display);
    for (auto ref : picture.references) (void)leases_.ReferenceUsed(ref, value);
    context.submitted_value = value;
    frame = {};
    frame.pts_100ns = packet.pts_100ns; frame.duration_100ns = packet.duration_100ns;
    frame.ready_fence_value = value; frame.ready_fence_id = options_.decode_timeline_id;
    frame.surface_id = handle.id; frame.surface_generation = handle.generation; frame.playback_epoch = epoch_;
    frame.color_metadata_id = picture.color_metadata_id;
    frame.coded_width = options_.width; frame.coded_height = options_.height;
    frame.crop_right = picture.crop_right ? picture.crop_right : options_.width;
    frame.crop_bottom = picture.crop_bottom ? picture.crop_bottom : options_.height;
    frame.crop_left = picture.crop_left; frame.crop_top = picture.crop_top;
    frame.format = options_.ten_bit ? core::VideoSurfaceFormat::P010 : core::VideoSurfaceFormat::Nv12;
    frame.flags = picture.display ? core::VideoFrameFlags::None : core::VideoFrameFlags::NoDisplay;
    return DecodeStatus::Submitted;
}

DecodeStatus D3D12VideoDecoder::CreateUpload() noexcept {
    // The surfaces as for decoding (committed NV12/P010 textures, the planes sampled by the converter), a copy queue and the
    // decode fence it signals, a copy list per surface, and kUploadSlots persistently mapped upload buffers of one picture.
    auto fail = [&](HRESULT hr) noexcept { error_ = hr; ReleaseObjects(); return DecodeStatus::Failed; };
    const auto& options = options_;
    const auto format = options.ten_bit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT feature{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    HRESULT hr = device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &feature, sizeof(feature));
    if (FAILED(hr) || !(feature.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D)) { error_ = FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED; ReleaseObjects(); return DecodeStatus::Unsupported; }
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = options.width; texture.Height = options.height;
    texture.DepthOrArraySize = 1; texture.MipLevels = 1; texture.Format = format; texture.SampleDesc.Count = 1;
    UINT64 bytes = 0;
    device_->GetCopyableFootprints(&texture, 0, 2, 0, footprints_, footprint_rows_, nullptr, &bytes);
    if (!bytes || bytes == UINT64_MAX) return fail(E_INVALIDARG);
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    hr = device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&copy_queue_));
    if (SUCCEEDED(hr)) hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence_));
    if (SUCCEEDED(hr)) hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&decode_fence_));
    if (FAILED(hr)) return fail(hr);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return fail(HRESULT_FROM_WIN32(GetLastError()));
    D3D12_HEAP_PROPERTIES local{}, upload{};
    local.Type = D3D12_HEAP_TYPE_DEFAULT; local.CreationNodeMask = local.VisibleNodeMask = 1;
    upload.Type = D3D12_HEAP_TYPE_UPLOAD; upload.CreationNodeMask = upload.VisibleNodeMask = 1;
    for (std::uint32_t i = 0; i < options.surfaces; ++i) {
        auto& context = contexts_[i];
        hr = device_->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&context.surface));
        if (SUCCEEDED(hr)) hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&context.copy_allocator));
        if (SUCCEEDED(hr)) hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, context.copy_allocator, nullptr, IID_PPV_ARGS(&context.copy_list));
        if (SUCCEEDED(hr)) hr = context.copy_list->Close();
        if (FAILED(hr)) return fail(hr);
    }
    const auto buffer = Buffer(bytes);
    for (auto& slot : uploads_) {
        hr = device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&slot.buffer));
        void* mapping = nullptr; const D3D12_RANGE no_read{0, 0};
        if (SUCCEEDED(hr)) hr = slot.buffer->Map(0, &no_read, &mapping);
        if (FAILED(hr)) return fail(hr);
        slot.mapped = static_cast<std::byte*>(mapping); slot.value = 0;
    }
    next_upload_ = 0;
    epoch_ = options_.epoch; prepared_ = true; return DecodeStatus::Ready;
}

DecodeStatus D3D12VideoDecoder::UploadImpl(const UploadPicture& picture, core::VideoFrameDescriptor& frame) noexcept {
    if (!prepared_ || faulted_ || options_.codec != VideoCodec::Upload) return DecodeStatus::Failed;
    if (!picture.planes[0] || !picture.planes[1] || !picture.width || !picture.height || picture.width > options_.width ||
        picture.height > options_.height || picture.pts_100ns == INT64_MIN || picture.duration_100ns < 0 || sequence_ >= UINT64_MAX - 2)
        return DecodeStatus::InvalidArgument;
    const unsigned sample = options_.ten_bit ? 2u : 1u;
    if (picture.pitches[0] < picture.width * sample || picture.pitches[1] < ((picture.width + 1) & ~1u) * sample) return DecodeStatus::InvalidArgument;
    auto& slot = uploads_[next_upload_];
    const auto copied = decode_fence_->GetCompletedValue();
    const auto consumed = consumer_fence_ ? consumer_fence_->GetCompletedValue() : 0;
    if (copied == UINT64_MAX || consumed == UINT64_MAX) return Result(DXGI_ERROR_DEVICE_REMOVED);
    if (copied < slot.value) return DecodeStatus::WouldBlock; // The copy that read this buffer last is still running.
    SurfaceHandle handle{};
    if (!leases_.Reserve(copied, consumed, handle)) return DecodeStatus::WouldBlock;
    auto& context = contexts_[handle.id];
    // The planes into the buffer at the texture's footprints (rows of RowPitch bytes): the one CPU copy of the path.
    for (unsigned plane = 0; plane < 2; ++plane) {
        const auto& footprint = footprints_[plane];
        const unsigned rows = plane ? (picture.height + 1) / 2 : picture.height;
        const std::size_t row_bytes = std::size_t{plane ? ((picture.width + 1) & ~1u) : picture.width} * sample;
        auto* target = slot.mapped + footprint.Offset;
        const auto* source = picture.planes[plane];
        if (picture.pitches[plane] == footprint.Footprint.RowPitch) std::memcpy(target, source, std::size_t{rows - 1} * footprint.Footprint.RowPitch + row_bytes);
        else for (unsigned y = 0; y < rows; ++y)
            std::memcpy(target + std::size_t{y} * footprint.Footprint.RowPitch, source + std::size_t{y} * picture.pitches[plane], row_bytes);
    }
    HRESULT hr = context.copy_allocator->Reset();
    if (SUCCEEDED(hr)) hr = context.copy_list->Reset(context.copy_allocator, nullptr);
    if (FAILED(hr)) { (void)leases_.Cancel(handle); return Result(hr); }
    auto barrier = Transition(context.surface, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    context.copy_list->ResourceBarrier(1, &barrier);
    for (UINT plane = 0; plane < 2; ++plane) {
        D3D12_TEXTURE_COPY_LOCATION to{context.surface, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        to.SubresourceIndex = plane;
        D3D12_TEXTURE_COPY_LOCATION from{slot.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        from.PlacedFootprint = footprints_[plane];
        const UINT width = plane ? (picture.width + 1) / 2 : picture.width, height = plane ? (picture.height + 1) / 2 : picture.height;
        const D3D12_BOX box{0, 0, 0, width, height, 1};
        context.copy_list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    }
    barrier = Transition(context.surface, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    context.copy_list->ResourceBarrier(1, &barrier);
    hr = context.copy_list->Close();
    if (FAILED(hr)) { (void)leases_.Cancel(handle); return Result(hr); }
    const auto value = ++sequence_;
    ID3D12CommandList* lists[]{context.copy_list}; copy_queue_->ExecuteCommandLists(1, lists);
    hr = copy_queue_->Signal(decode_fence_, value);
    if (FAILED(hr)) return Result(hr);
    slot.value = value; next_upload_ = (next_upload_ + 1) % kUploadSlots;
    (void)leases_.Submitted(handle, value, false, !picture.display);
    context.submitted_value = value;
    frame = {};
    frame.pts_100ns = picture.pts_100ns; frame.duration_100ns = picture.duration_100ns;
    frame.ready_fence_value = value; frame.ready_fence_id = options_.decode_timeline_id;
    frame.surface_id = handle.id; frame.surface_generation = handle.generation; frame.playback_epoch = epoch_;
    frame.color_metadata_id = picture.color_metadata_id;
    frame.coded_width = options_.width; frame.coded_height = options_.height;
    frame.crop_right = picture.crop_right ? picture.crop_right : static_cast<std::uint16_t>(picture.width);
    frame.crop_bottom = picture.crop_bottom ? picture.crop_bottom : static_cast<std::uint16_t>(picture.height);
    frame.crop_left = picture.crop_left; frame.crop_top = picture.crop_top;
    frame.format = options_.ten_bit ? core::VideoSurfaceFormat::P010 : core::VideoSurfaceFormat::Nv12;
    frame.flags = picture.display ? core::VideoFrameFlags::None : core::VideoFrameFlags::NoDisplay;
    return DecodeStatus::Submitted;
}

bool D3D12VideoDecoder::ReturnSurface(VideoSurfaceReturn returned) noexcept {
    if (returned.epoch < options_.epoch || returned.epoch > epoch_ || returned.timeline_id != options_.consumer_timeline_id ||
        (!consumer_fence_ && returned.fence_value)) return false;
    if (!leases_.Return({returned.id, returned.generation}, returned.fence_value)) return false;
    consumer_high_water_ = (std::max)(consumer_high_water_, returned.fence_value); return true;
}
ID3D12Resource* D3D12VideoDecoder::Surface(SurfaceHandle handle) const noexcept {
    return leases_.Valid(handle) ? contexts_[handle.id].surface : nullptr;
}
DecodeStatus D3D12VideoDecoder::Statistics(SurfaceHandle handle, D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS& out) noexcept {
    if (!prepared_ || !leases_.Valid(handle) || !contexts_[handle.id].submitted_value) return DecodeStatus::InvalidArgument;
    const auto complete = decode_fence_->GetCompletedValue();
    if (complete == UINT64_MAX) return Result(DXGI_ERROR_DEVICE_REMOVED);
    if (complete < contexts_[handle.id].submitted_value) return DecodeStatus::WouldBlock;
    if (!statistics_) { out = {}; out.Status = D3D12_VIDEO_DECODE_STATUS_OK; return DecodeStatus::Ready; } // Upload: a copy, nothing to report.
    std::memcpy(&out, statistics_ + handle.id * sizeof(out), sizeof(out));
    return DecodeStatus::Ready;
}
bool D3D12VideoDecoder::Wait(ID3D12Fence* fence, std::uint64_t value) noexcept {
    if (!fence || !value) return true;
    if (fence->GetCompletedValue() >= value) return true;
    const auto hr = fence->SetEventOnCompletion(value, event_);
    if (FAILED(hr)) { error_ = hr; return false; }
    for (;;) {
        const auto state = WaitForSingleObject(event_, 100);
        if (state == WAIT_OBJECT_0) return true;
        if (state != WAIT_TIMEOUT || FAILED(device_->GetDeviceRemovedReason())) return false;
    }
}
DecodeStatus D3D12VideoDecoder::DrainAndClose() noexcept {
    if (quarantined_) return DecodeStatus::Failed;
    if (!device_) return DecodeStatus::Ready;
    if (SUCCEEDED(device_->GetDeviceRemovedReason()) && !leases_.AllConsumersReturned()) return DecodeStatus::WouldBlock;
    // All callers/consumer queues must be quiescent before teardown. Fresh queue
    // markers also cover failures after ExecuteCommandLists but before its Signal.
    if (sequence_ && SUCCEEDED(device_->GetDeviceRemovedReason())) {
        const auto value = ++sequence_;
        HRESULT hr = copy_queue_->Signal(copy_fence_, value);
        if (SUCCEEDED(hr)) hr = decode_queue_ ? decode_queue_->Signal(decode_fence_, value) : copy_queue_->Signal(decode_fence_, value);
        if (FAILED(hr) || !Wait(copy_fence_, value) || !Wait(decode_fence_, value) || !Wait(consumer_fence_, consumer_high_water_)) {
            if (SUCCEEDED(device_->GetDeviceRemovedReason())) return Result(FAILED(hr) ? hr : E_FAIL);
        }
    }
    ReleaseObjects(); return DecodeStatus::Ready;
}
bool D3D12VideoDecoder::Compatible(ID3D12Device* device, ID3D12Fence* consumer, const DecoderOptions& o) const noexcept {
    const auto& c = options_;
    return prepared_ && !faulted_ && !quarantined_ && device && device == device_ && consumer == consumer_fence_ && o.codec == c.codec &&
           o.width == c.width && o.height == c.height && o.surfaces == c.surfaces && o.max_bitstream_bytes == c.max_bitstream_bytes &&
           o.ten_bit == c.ten_bit && o.decode_timeline_id == c.decode_timeline_id && o.consumer_timeline_id == c.consumer_timeline_id;
}
DecodeStatus D3D12VideoDecoder::Restart(std::uint32_t epoch) noexcept {
    if (quarantined_ || faulted_ || !prepared_ || !epoch) return DecodeStatus::InvalidArgument;
    if (FAILED(device_->GetDeviceRemovedReason())) return Result(device_->GetDeviceRemovedReason());
    if (!leases_.AllConsumersReturned()) return DecodeStatus::WouldBlock;
    // As DrainAndClose: fresh markers on both queues, and the consumer's high water, all retired.
    if (sequence_) {
        const auto value = ++sequence_;
        HRESULT hr = copy_queue_->Signal(copy_fence_, value);
        if (SUCCEEDED(hr)) hr = decode_queue_ ? decode_queue_->Signal(decode_fence_, value) : copy_queue_->Signal(decode_fence_, value);
        if (FAILED(hr) || !Wait(copy_fence_, value) || !Wait(decode_fence_, value) || !Wait(consumer_fence_, consumer_high_water_))
            return Result(FAILED(hr) ? hr : E_FAIL);
    }
    leases_.Reset();
    for (std::uint32_t i = 0; i < options_.surfaces; ++i) contexts_[i].submitted_value = 0;
    for (auto& slot : uploads_) slot.value = 0;
    options_.epoch = epoch; epoch_ = epoch; // sequence_ and consumer_high_water_ go on: the fences are the same ones.
    return DecodeStatus::Ready;
}
DecodeStatus D3D12VideoDecoder::Close() noexcept {
    const auto status = DrainAndClose();
    if (status != DecodeStatus::Ready) return status;
    contexts_ = nullptr; leases_.Forget(); options_ = {}; epoch_ = 0; faulted_ = false;
    return DecodeStatus::Ready;
}
void D3D12VideoDecoder::ReleaseObjects() noexcept {
    prepared_ = false;
    if (contexts_) for (std::uint32_t i = 0; i < options_.surfaces; ++i) {
        auto& context = contexts_[i];
        Release(context.decode_list); Release(context.copy_list);
        Release(context.decode_allocator); Release(context.copy_allocator);
        if (context.upload && context.mapped) context.upload->Unmap(0, nullptr);
        context.mapped = nullptr; Release(context.upload); Release(context.bitstream); Release(context.surface);
    }
    for (auto& slot : uploads_) {
        if (slot.buffer && slot.mapped) slot.buffer->Unmap(0, nullptr);
        slot.mapped = nullptr; slot.value = 0; Release(slot.buffer);
    }
    if (statistics_buffer_ && statistics_) { const D3D12_RANGE no_write{0, 0}; statistics_buffer_->Unmap(0, &no_write); }
    statistics_ = nullptr; Release(statistics_buffer_); Release(query_heap_);
    Release(decoder_heap_); Release(decoder_); Release(bitstream_heap_);
    Release(copy_queue_); Release(decode_queue_); Release(copy_fence_); Release(decode_fence_); Release(consumer_fence_);
    Release(video_); Release(device_);
    if (event_) { CloseHandle(event_); event_ = nullptr; }
}
}
