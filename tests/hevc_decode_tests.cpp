// HEVC DXVA adapter checks and hardware decode verification (HEVC and H.264).
// Fixture authoring uses the platform's own HEVC encoder MFT (no bundled media or third-party tools).
// Encoding and verification-resource setup allocate before sealing; the decode loop is measured.
#include "media/platform/windows/HevcDxva.hpp"
#include "media/platform/windows/H264Dxva.hpp"
#include "media/video/ReorderWindow.hpp"
#include "allocation_probe.hpp"
#include "HevcCompareShader.hpp"
#include "platform_hevc_encoder.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace media;
using namespace media::platform;
using namespace media::platform::windows;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---- CPU-only: HevcDxva field mapping ----
int Dxva() {
    codec::HevcSps s{};
    s.valid = true; s.width = 320; s.height = 192; s.right = 318; s.bottom = 190;
    s.bit_depth_luma_minus8 = s.bit_depth_chroma_minus8 = 2; s.poc_lsb_bits = 8; s.max_dec_pic_buffering_minus1 = 4;
    s.log2_min_cb_minus3 = 0; s.log2_diff_max_min_cb = 2; s.log2_min_tb_minus2 = 0; s.log2_diff_max_min_tb = 3;
    s.max_transform_depth_inter = 2; s.max_transform_depth_intra = 1;
    s.pcm = true; s.pcm_bit_depth_luma_minus1 = 9; s.pcm_bit_depth_chroma_minus1 = 8; s.log2_min_pcm_minus3 = 1;
    s.log2_diff_max_min_pcm = 1; s.pcm_loop_filter_disabled = true;
    s.scaling_list_enabled = true; s.amp = true; s.sao = true; s.long_term_present = true; s.temporal_mvp = true;
    s.strong_intra_smoothing = true; s.num_short_term_rps = 5; s.num_long_term_sps = 2;
    s.matrix = 9; s.full_range = true; s.transfer = 16; s.colour_primaries = 9;
    for (unsigned size = 0; size < 4; ++size)
        for (unsigned m = 0; m < 6; ++m)
            for (unsigned i = 0; i < 64; ++i) s.scaling.lists[size][m][i] = static_cast<std::uint8_t>(size * 50 + m * 8 + i % 8 + 1);
    for (unsigned m = 0; m < 6; ++m) { s.scaling.dc[0][m] = static_cast<std::uint8_t>(100 + m); s.scaling.dc[1][m] = static_cast<std::uint8_t>(200 + m); }
    codec::HevcPps p{};
    p.valid = true; p.refs_l0_minus1 = 3; p.refs_l1_minus1 = 2; p.init_qp_minus26 = -5; p.cb_qp_offset = -3; p.cr_qp_offset = 4;
    p.tiles = true; p.uniform_spacing = false; p.tile_columns_minus1 = 2; p.tile_rows_minus1 = 1;
    p.column_width_minus1[0] = 3; p.column_width_minus1[1] = 4; p.row_height_minus1[0] = 5;
    p.beta_offset_div2 = -2; p.tc_offset_div2 = 3; p.log2_parallel_merge_minus2 = 2; p.cu_qp_delta = true; p.diff_cu_qp_delta_depth = 1;
    p.extra_slice_header_bits = 5; p.dependent_slices = true; p.sign_data_hiding = true; p.weighted_pred = true;
    p.loop_filter_across_tiles = false; p.deblocking_override = true;
    codec::HevcPicture pic{};
    pic.sps = &s; pic.pps = &p; pic.poc = 37; pic.short_term_rps_bits = 23; pic.reference_rps_delta_pocs = 3;
    pic.intra = false; pic.retain = true;
    pic.references[0] = {{5, 2}, 36, false}; pic.references[1] = {{7, 1}, 40, false}; pic.references[2] = {{9, 4}, 16, true};
    pic.reference_count = 3;
    pic.st_curr_before[0] = 0; pic.st_curr_before_count = 1; pic.st_curr_after[0] = 1; pic.st_curr_after_count = 1;
    pic.lt_curr[0] = 2; pic.lt_curr_count = 1;
    pic.slices[0] = {4, 100}; pic.slices[1] = {108, 50}; pic.slice_count = 2;
    static codec::HevcDxva dxva; // Large DXVA records; static storage keeps the test stack small.
    core::MediaPacket packet{}; packet.payload_bytes = 158;
    std::byte bytes[158]{};
    const auto out = dxva.Prepare(pic, packet, {bytes, 0, 158, {2, 1}});
    const auto& d = dxva.parameters;
    CHECK(d.PicWidthInMinCbsY == 40 && d.PicHeightInMinCbsY == 24 && d.chroma_format_idc == 1 && !d.separate_colour_plane_flag);
    CHECK(d.bit_depth_luma_minus8 == 2 && d.bit_depth_chroma_minus8 == 2 && d.log2_max_pic_order_cnt_lsb_minus4 == 4);
    CHECK(d.NoPicReorderingFlag == 1 && d.NoBiPredFlag == 0 && d.sps_max_dec_pic_buffering_minus1 == 4);
    CHECK(d.log2_min_luma_coding_block_size_minus3 == 0 && d.log2_diff_max_min_luma_coding_block_size == 2);
    CHECK(d.log2_min_transform_block_size_minus2 == 0 && d.log2_diff_max_min_transform_block_size == 3);
    CHECK(d.max_transform_hierarchy_depth_inter == 2 && d.max_transform_hierarchy_depth_intra == 1);
    CHECK(d.num_short_term_ref_pic_sets == 5 && d.num_long_term_ref_pics_sps == 2);
    CHECK(d.num_ref_idx_l0_default_active_minus1 == 3 && d.num_ref_idx_l1_default_active_minus1 == 2 && d.init_qp_minus26 == -5);
    CHECK(d.ucNumDeltaPocsOfRefRpsIdx == 3 && d.wNumBitsForShortTermRPSInSlice == 23);
    CHECK(d.scaling_list_enabled_flag && d.amp_enabled_flag && d.sample_adaptive_offset_enabled_flag && d.pcm_enabled_flag);
    CHECK(d.pcm_sample_bit_depth_luma_minus1 == 9 && d.pcm_sample_bit_depth_chroma_minus1 == 8);
    CHECK(d.log2_min_pcm_luma_coding_block_size_minus3 == 1 && d.log2_diff_max_min_pcm_luma_coding_block_size == 1 && d.pcm_loop_filter_disabled_flag);
    CHECK(d.long_term_ref_pics_present_flag && d.sps_temporal_mvp_enabled_flag && d.strong_intra_smoothing_enabled_flag);
    CHECK(d.dependent_slice_segments_enabled_flag && !d.output_flag_present_flag && d.num_extra_slice_header_bits == 5);
    CHECK(d.sign_data_hiding_enabled_flag && !d.cabac_init_present_flag && d.cu_qp_delta_enabled_flag && d.weighted_pred_flag);
    CHECK(d.tiles_enabled_flag && !d.uniform_spacing_flag && !d.loop_filter_across_tiles_enabled_flag && d.deblocking_filter_override_enabled_flag);
    CHECK(!d.IrapPicFlag && !d.IdrPicFlag && !d.IntraPicFlag);
    CHECK(d.pps_cb_qp_offset == -3 && d.pps_cr_qp_offset == 4 && d.num_tile_columns_minus1 == 2 && d.num_tile_rows_minus1 == 1);
    CHECK(d.column_width_minus1[0] == 3 && d.column_width_minus1[1] == 4 && d.column_width_minus1[2] == 0);
    CHECK(d.row_height_minus1[0] == 5 && d.row_height_minus1[1] == 0);
    CHECK(d.diff_cu_qp_delta_depth == 1 && d.pps_beta_offset_div2 == -2 && d.pps_tc_offset_div2 == 3 && d.log2_parallel_merge_level_minus2 == 2);
    CHECK(d.CurrPicOrderCntVal == 37);
    CHECK(d.RefPicList[0].bPicEntry == 0 && d.RefPicList[1].bPicEntry == 1 && d.RefPicList[2].bPicEntry == 0x82 && d.RefPicList[3].bPicEntry == 0xFF);
    CHECK(d.RefPicList[2].Index7Bits == 2 && d.RefPicList[2].AssociatedFlag == 1 && d.RefPicList[14].bPicEntry == 0xFF);
    CHECK(d.PicOrderCntValList[0] == 36 && d.PicOrderCntValList[1] == 40 && d.PicOrderCntValList[2] == 16);
    CHECK(d.RefPicSetStCurrBefore[0] == 0 && d.RefPicSetStCurrBefore[1] == 0xFF && d.RefPicSetStCurrAfter[0] == 1 &&
          d.RefPicSetStCurrAfter[7] == 0xFF && d.RefPicSetLtCurr[0] == 2 && d.RefPicSetLtCurr[1] == 0xFF);
    bool lists = true;
    for (unsigned m = 0; m < 6; ++m) {
        lists = lists && std::memcmp(dxva.matrix.ucScalingLists0[m], s.scaling.lists[0][m], 16) == 0 &&
                std::memcmp(dxva.matrix.ucScalingLists1[m], s.scaling.lists[1][m], 64) == 0 &&
                std::memcmp(dxva.matrix.ucScalingLists2[m], s.scaling.lists[2][m], 64) == 0 &&
                dxva.matrix.ucScalingListDCCoefSizeID2[m] == 100 + m;
    }
    lists = lists && std::memcmp(dxva.matrix.ucScalingLists3[0], s.scaling.lists[3][0], 64) == 0 &&
            std::memcmp(dxva.matrix.ucScalingLists3[1], s.scaling.lists[3][3], 64) == 0 &&
            dxva.matrix.ucScalingListDCCoefSizeID3[0] == 200 && dxva.matrix.ucScalingListDCCoefSizeID3[1] == 203;
    CHECK(lists);
    CHECK(out.hevc == &dxva.parameters && out.hevc_matrix == &dxva.matrix && !out.h264 && !out.h264_matrix);
    CHECK(out.references.size() == 3 && out.references[0].id == 5 && out.references[0].generation == 2 && out.references[2].id == 9);
    CHECK(out.slices.size() == 2 && out.slices[0].offset == 4 && out.slices[0].bytes == 100 && out.slices[1].offset == 108);
    CHECK(out.crop_left == 0 && out.crop_top == 0 && out.crop_right == 318 && out.crop_bottom == 190 && out.retain_as_reference);
    CHECK(out.color_metadata_id == (9u | 256u | (16u << 16) | (9u << 24)) && out.payload.valid_bytes == 158);
    // PPS scaling lists override the SPS lists; uniform tile spacing leaves explicit sizes zero.
    p.scaling_list_present = true; p.uniform_spacing = true;
    for (unsigned m = 0; m < 6; ++m) std::memset(p.scaling.lists[1][m], 77, 64);
    (void)dxva.Prepare(pic, packet, {bytes, 0, 158, {2, 1}});
    CHECK(dxva.matrix.ucScalingLists1[4][63] == 77 && dxva.parameters.column_width_minus1[0] == 0 && dxva.parameters.uniform_spacing_flag);
    pic.irap = pic.idr = pic.intra = true; pic.reference_count = 0; pic.st_curr_before_count = pic.st_curr_after_count = pic.lt_curr_count = 0;
    const auto idr = dxva.Prepare(pic, packet, {bytes, 0, 158, {2, 1}});
    CHECK(dxva.parameters.IrapPicFlag && dxva.parameters.IdrPicFlag && dxva.parameters.IntraPicFlag && idr.references.empty());
    CHECK(dxva.parameters.RefPicList[0].bPicEntry == 0xFF && dxva.parameters.RefPicSetStCurrBefore[0] == 0xFF);
    std::printf("HEVC DXVA mapping: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

// ---- Hardware: fixture authoring through the platform HEVC encoder (tests/platform_hevc_encoder.hpp) ----
constexpr unsigned kWidth = 320, kHeight = 192, kFrames = 48;
constexpr LONGLONG kFrameDuration = 333333;
using encoder::Encoded; using encoder::Fixture; using encoder::ForEachNal;
std::uint8_t Pattern(unsigned x, unsigned y, unsigned f) { // Must match HevcPatternCompare.hlsl.
    unsigned value = 40 + ((x + 2 * y + 13 * f) & 127);
    const unsigned bx = (5 * f) % (kWidth - 32), by = (3 * f) % (kHeight - 32);
    if (x - bx < 32 && y - by < 32) value += 60;
    return static_cast<std::uint8_t>(value);
}
void FillRow(unsigned frame, unsigned y, unsigned width, unsigned, std::uint8_t* row) {
    for (unsigned x = 0; x < width; ++x) row[x] = Pattern(x, y, frame);
}
Fixture Encode(bool ten_bit, std::vector<Encoded>& stream, bool& b_frames_requested) {
    encoder::Options options; options.width = kWidth; options.height = kHeight; options.frames = kFrames; options.fps = 30; options.ten_bit = ten_bit;
    return encoder::Encode(options, FillRow, stream, b_frames_requested);
}

// ---- Hardware: GPU pattern comparison (one scalar per frame read back, never pixels) ----
struct Verifier {
    static constexpr unsigned Slots = 16;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource *result = nullptr, *readback = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocators[Slots]{};
    ID3D12GraphicsCommandList* lists[Slots]{};
    std::uint64_t slot_values[Slots]{}, value = 0;
    UINT increment = 0;
    HRESULT Create(ID3D12Device* device) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[0].DescriptorTable = {1, &range};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[1].Descriptor = {0, 0};
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[2].Constants = {0, 0, 8};
        const D3D12_ROOT_SIGNATURE_DESC desc{3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *blob = nullptr, *error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        Release(blob); Release(error);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root; pso.CS = {g_HevcCompareShader, sizeof(g_HevcCompareShader)};
        if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
        increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = kFrames * 8;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT; // Committed buffers start zeroed.
        if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result));
        buffer.Flags = D3D12_RESOURCE_FLAG_NONE; properties.Type = D3D12_HEAP_TYPE_READBACK;
        if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        for (unsigned i = 0; i < Slots && SUCCEEDED(hr); ++i) {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i]));
            if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i], nullptr, IID_PPV_ARGS(&lists[i]));
            if (SUCCEEDED(hr)) hr = lists[i]->Close();
        }
        return hr;
    }
    void Bind(ID3D12Device* device, const D3D12VideoDecoder& decoder, std::uint32_t surfaces, bool ten_bit) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM; // Luma plane.
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t i = 0; i < surfaces; ++i, cpu.ptr += increment) device->CreateShaderResourceView(decoder.SurfaceAt(i), &srv, cpu);
    }
    bool Wait(std::uint64_t target) const {
        const auto deadline = GetTickCount64() + 10000;
        while (fence->GetCompletedValue() < target) { if (GetTickCount64() > deadline) return false; YieldProcessor(); }
        return fence->GetCompletedValue() != UINT64_MAX;
    }
    // Records one comparison for the frame against pattern `index`, plus a sensitivity control against index + 1.
    HRESULT Compare(ID3D12Resource* surface, std::uint32_t surface_id, ID3D12Fence* decoded, std::uint64_t ready,
                    unsigned frame, unsigned index, unsigned width, unsigned height, bool ten_bit, std::uint64_t& signalled) {
        const unsigned slot = frame % Slots;
        if (!Wait(slot_values[slot])) return E_FAIL;
        auto* list = lists[slot];
        HRESULT hr = allocators[slot]->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(allocators[slot], pipeline);
        if (FAILED(hr)) return hr;
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {surface, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        list->ResourceBarrier(1, &barrier);
        list->SetComputeRootSignature(root); list->SetDescriptorHeaps(1, &heap);
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += static_cast<UINT64>(surface_id) * increment;
        list->SetComputeRootDescriptorTable(0, gpu);
        list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
        UINT constants[8]{width, height, index, frame * 8, ten_bit ? 1u : 0u, 0, 0, 0};
        list->SetComputeRoot32BitConstants(2, 8, constants, 0); list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        constants[2] = index + 1; constants[3] = frame * 8 + 4;
        list->SetComputeRoot32BitConstants(2, 8, constants, 0); list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        hr = list->Close();
        if (SUCCEEDED(hr)) hr = queue->Wait(decoded, ready); // GPU-side dependency on the decode timeline.
        if (FAILED(hr)) return hr;
        ID3D12CommandList* submit[]{list}; queue->ExecuteCommandLists(1, submit);
        hr = queue->Signal(fence, ++value);
        slot_values[slot] = value; signalled = value;
        return hr;
    }
    HRESULT Read(unsigned frames, std::uint32_t* sums) {
        if (!Wait(value)) return E_FAIL;
        auto* list = lists[0];
        HRESULT hr = allocators[0]->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(allocators[0], nullptr);
        if (FAILED(hr)) return hr;
        list->CopyBufferRegion(readback, 0, result, 0, frames * 8ull);
        hr = list->Close();
        if (FAILED(hr)) return hr;
        ID3D12CommandList* submit[]{list}; queue->ExecuteCommandLists(1, submit);
        hr = queue->Signal(fence, ++value);
        if (FAILED(hr) || !Wait(value)) return FAILED(hr) ? hr : E_FAIL;
        void* mapped = nullptr; const D3D12_RANGE range{0, frames * 8ull};
        hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(hr)) { std::memcpy(sums, mapped, frames * 8ull); const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none); }
        return hr;
    }
    void Destroy() {
        if (fence && value) (void)Wait(value);
        for (auto& list : lists) Release(list);
        for (auto& allocator : allocators) Release(allocator);
        Release(fence); Release(queue); Release(readback); Release(result); Release(heap); Release(pipeline); Release(root);
    }
};

int DecodeRun(ID3D12Device* device, const std::vector<Encoded>& stream, bool expect_ten_bit) {
    const int failures_before = failures;
    core::LinearMemoryArena arena; CHECK(arena.Initialize(16 * 1024 * 1024));
    codec::HevcSyntaxParser parser; CHECK(parser.Initialize(arena));
    auto* dxva = arena.Create<codec::HevcDxva>();
    auto* frames = arena.Create<core::SpscRingBuffer<core::VideoFrameDescriptor, 16>>();
    auto* returns = arena.Create<core::SpscRingBuffer<core::MediaPacket, 16>>();
    std::size_t largest = 0; for (const auto& e : stream) largest = (std::max)(largest, e.au.size());
    const auto capacity = (largest + 127) & ~std::size_t{127};
    auto* payload = static_cast<std::byte*>(arena.Allocate(capacity));
    CHECK(dxva && frames && returns && payload);
    if (!dxva || !frames || !returns || !payload) return 1;
    // Preparation: the first access unit's parameter sets configure the decoder.
    ForEachNal(stream.front().au, [&](std::span<const std::byte> nal) {
        const auto type = (std::to_integer<unsigned>(nal[0]) >> 1) & 63;
        if (type >= 32 && type <= 34) CHECK(parser.ParameterSet(nal) == codec::SyntaxStatus::Ready);
    });
    const auto* sps = parser.FirstSps(); CHECK(sps != nullptr);
    if (!sps) return 1;
    const bool ten_bit = sps->bit_depth_luma_minus8 == 2;
    const unsigned width = sps->right - sps->left, height = sps->bottom - sps->top;
    CHECK(ten_bit == expect_ten_bit && sps->left == 0 && sps->top == 0 && width == kWidth && height == kHeight);
    Verifier verifier{};
    if (const auto hr = verifier.Create(device); FAILED(hr)) {
        std::printf("FAIL verification pipeline: %08lx\n", static_cast<unsigned long>(hr)); verifier.Destroy(); return 1;
    }
    D3D12VideoDecoder decoder;
    DecoderOptions options; options.codec = VideoCodec::Hevc; options.ten_bit = ten_bit;
    options.width = sps->width; options.height = sps->height; options.surfaces = 16;
    options.max_bitstream_bytes = static_cast<std::uint32_t>((capacity + 3 * 256 + 127) & ~std::size_t{127});
    const auto prepared = decoder.Prepare(arena, device, verifier.fence, options);
    if (prepared == DecodeStatus::Unsupported) {
        std::printf("SKIP HEVC %s tier 2 decode unavailable: %08lx\n", ten_bit ? "Main10" : "Main", static_cast<unsigned long>(decoder.LastError()));
        verifier.Destroy(); return failures > failures_before ? 1 : 77;
    }
    if (prepared != DecodeStatus::Ready) {
        std::printf("FAIL HEVC decoder prepare: %08lx\n", static_cast<unsigned long>(decoder.LastError())); ++failures;
        verifier.Destroy(); return 1;
    }
    verifier.Bind(device, decoder, options.surfaces, ten_bit);
    CHECK(arena.Seal());
    const auto arena_allocations = arena.Statistics().allocation_count;
    unsigned processed = 0, submitted_count = 0, discarded = 0, irap = 0, bad_status = 0, seen[kFrames]{}, gop_base = 0;
    struct Trace { int poc; unsigned display, nal, rps_bits, delta_pocs, references; };
    Trace trace[kFrames]{};
    bool reordered = false; LONGLONG last_pts = LLONG_MIN;
    auto consume = [&]() -> bool {
        const auto* frame = frames->PeekReadSlot();
        if (!frame) return false;
        const SurfaceHandle handle{frame->surface_id, frame->surface_generation};
        const auto index = static_cast<unsigned>((frame->pts_100ns + kFrameDuration / 2) / kFrameDuration);
        if (index < kFrames) ++seen[index];
        // The comparison waits for the decode on the GPU timeline, as a renderer would; the CPU only polls status.
        std::uint64_t signalled = 0;
        const auto compared = processed < kFrames ? verifier.Compare(decoder.SurfaceAt(frame->surface_id), frame->surface_id,
            decoder.DecodeFence(), frame->ready_fence_value, processed, index, width, height, ten_bit, signalled) : E_BOUNDS;
        CHECK(SUCCEEDED(compared));
        D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS stats{};
        auto state = DecodeStatus::WouldBlock; const auto deadline = GetTickCount64() + 10000;
        while (state == DecodeStatus::WouldBlock && GetTickCount64() < deadline) { state = decoder.Statistics(handle, stats); YieldProcessor(); }
        if (state != DecodeStatus::Ready || stats.Status != D3D12_VIDEO_DECODE_STATUS_OK || stats.NumMacroblocksAffected) ++bad_status;
        CHECK(decoder.ReturnSurface({handle.id, handle.generation, frame->playback_epoch, options.consumer_timeline_id, signalled}));
        (void)frames->ReleaseReadSlot(); ++processed; return true;
    };
    allocation_probe::Begin();
    for (const auto& e : stream) {
        std::memcpy(payload, e.au.data(), e.au.size()); // Stand-in for the demuxer's leased block.
        const std::span<const std::byte> bytes{payload, e.au.size()};
        const auto status = parser.Parse(bytes, 4);
        if (status == codec::SyntaxStatus::Discard) { ++discarded; continue; }
        CHECK(status == codec::SyntaxStatus::Ready);
        if (status != codec::SyntaxStatus::Ready) break;
        const auto* picture = parser.Picture();
        irap += picture->irap ? 1u : 0u;
        // The encoder labels output samples in decode order; closed-GOP display order is IDR base + POC.
        if (picture->idr) gop_base = submitted_count;
        const auto display = gop_base + static_cast<unsigned>(picture->poc);
        const auto pts = static_cast<LONGLONG>(display) * kFrameDuration;
        reordered = reordered || pts < last_pts; last_pts = pts;
        if (submitted_count < kFrames) trace[submitted_count] = {picture->poc, display, picture->nal_type, picture->short_term_rps_bits,
                                                                 picture->reference_rps_delta_pocs, picture->reference_count};
        core::MediaPacket packet{};
        packet.pts_100ns = pts; packet.dts_100ns = e.pts; packet.duration_100ns = kFrameDuration;
        packet.payload_bytes = static_cast<std::uint32_t>(e.au.size()); packet.payload_block_id = 2;
        packet.payload_generation = 1; packet.playback_epoch = 1;
        const auto native = dxva->Prepare(*picture, packet, {payload, 0, packet.payload_bytes, {2, 1}});
        SurfaceHandle surface{};
        auto decoded = DecodeStatus::WouldBlock;
        for (unsigned spin = 0; spin < 1000000 && decoded == DecodeStatus::WouldBlock; ++spin) {
            decoded = decoder.Submit(native, *frames, *returns, &surface);
            if (decoded == DecodeStatus::WouldBlock && !consume()) YieldProcessor();
        }
        CHECK(decoded == DecodeStatus::Submitted);
        if (decoded != DecodeStatus::Submitted) { parser.Cancel(); break; }
        while (returns->PeekReadSlot()) (void)returns->ReleaseReadSlot();
        SurfaceHandle evicted[16]{}; unsigned count = 0;
        CHECK(parser.Commit(surface, evicted, count));
        for (unsigned i = 0; i < count; ++i) CHECK(decoder.ReleaseReference(evicted[i]));
        ++submitted_count;
        while (consume()) {}
    }
    while (consume()) {}
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0 && arena.Statistics().allocation_count == arena_allocations);
    CHECK(submitted_count == stream.size() && discarded == 0 && processed == submitted_count && bad_status == 0 && irap >= 2);
    bool complete = true; for (auto n : seen) complete = complete && n == 1;
    CHECK(complete);
    std::uint32_t sums[kFrames * 2]{};
    const auto read = verifier.Read(processed < kFrames ? processed : kFrames, sums);
    CHECK(SUCCEEDED(read));
    double worst = 0, weakest_control = 1e9;
    const double pixels = static_cast<double>(width) * height;
    for (unsigned i = 0; SUCCEEDED(read) && i < processed && i < kFrames; ++i) {
        const double error = sums[2 * i] / pixels, control = sums[2 * i + 1] / pixels;
        if (error >= 4.0 || control <= 8.0)
            std::printf("FAIL decode-order %u (display %u, POC %d, nal %u, refs %u, rps bits %u, ref delta POCs %u): error %.3f, control %.3f\n",
                        i, trace[i].display, trace[i].poc, trace[i].nal, trace[i].references, trace[i].rps_bits, trace[i].delta_pocs, error, control);
        CHECK(error < 4.0 && control > 8.0);
        worst = (std::max)(worst, error); weakest_control = (std::min)(weakest_control, control);
    }
    std::printf("HEVC %s GPU decode: %u pictures, %u IRAP, reordering %s, decode status OK=%s, "
                "max mean |luma error| %.3f (control >= %.3f), allocations %llu\n",
                ten_bit ? "Main10" : "Main", processed, irap, reordered ? "present" : "absent", bad_status ? "no" : "yes",
                worst, weakest_control, static_cast<unsigned long long>(allocations));
    CHECK(decoder.DrainAndClose() == DecodeStatus::Ready);
    verifier.Destroy();
    return failures > failures_before ? 1 : 0;
}
// ---- Hardware: native backend reference-read regression ----
// Handwritten 64x64 H.264 (no encoder needed): an I_PCM IDR (flat luma 128), a P picture made of
// I_PCM macroblocks (flat 64) and a P-skip picture that must copy its reference. A status-only check
// cannot see a reference that motion compensation reads as zeros; these pixel sums can.
struct ProbeBits {
    std::array<std::byte, 8192> data{}; unsigned bits = 0;
    void Bit(unsigned v) { if (v) data[bits / 8] |= std::byte(1u << (7 - bits % 8)); ++bits; }
    void Value(unsigned v, unsigned n) { while (n) { --n; Bit((v >> n) & 1); } }
    void Ue(unsigned v) { const unsigned code = v + 1; unsigned n = 0; for (unsigned t = code; t > 1; t >>= 1) ++n; for (unsigned i = 0; i < n; ++i) Bit(0); Value(code, n + 1); }
    void Pcm(unsigned luma) { while (bits % 8) Bit(0); for (unsigned i = 0; i != 256; ++i) Value(luma, 8); for (unsigned i = 0; i != 128; ++i) Value(128, 8); }
    unsigned Nal(unsigned header, std::byte* out) {
        Bit(1); while (bits % 8) Bit(0);
        unsigned count = 0, zeros = 0; out[count++] = static_cast<std::byte>(header);
        for (unsigned i = 0; i != bits / 8; ++i) {
            const auto v = std::to_integer<unsigned>(data[i]);
            if (zeros == 2 && v <= 3) { out[count++] = std::byte{3}; zeros = 0; }
            out[count++] = data[i]; zeros = v == 0 ? zeros + 1 : 0;
        }
        return count;
    }
};
std::uint32_t FlatSum(unsigned value) { // Shader sum for a flat 64x64 luma plane against pattern frame 0.
    std::uint32_t sum = 0;
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        unsigned v = 40 + ((x + 2 * y) & 127); if (x < 32 && y < 32) v += 60;
        sum += v > value ? v - value : value - v;
    }
    return sum;
}
int ReferenceRun(ID3D12Device* device) {
    const int failures_before = failures;
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    auto* frames = arena.Create<core::SpscRingBuffer<core::VideoFrameDescriptor, 16>>();
    auto* returns = arena.Create<core::SpscRingBuffer<core::MediaPacket, 16>>();
    auto* payload = static_cast<std::byte*>(arena.Allocate(8192));
    if (!frames || !returns || !payload) return 1;
    Verifier verifier{};
    if (FAILED(verifier.Create(device))) { verifier.Destroy(); return 1; }
    D3D12VideoDecoder decoder; DecoderOptions options;
    options.width = options.height = 64; options.surfaces = 4; options.max_bitstream_bytes = 8192;
    const auto prepared = decoder.Prepare(arena, device, verifier.fence, options);
    if (prepared != DecodeStatus::Ready) {
        std::printf("%s H.264 decoder prepare: %08lx\n", prepared == DecodeStatus::Unsupported ? "SKIP" : "FAIL", static_cast<unsigned long>(decoder.LastError()));
        verifier.Destroy(); return prepared == DecodeStatus::Unsupported ? 77 : 1;
    }
    verifier.Bind(device, decoder, options.surfaces, false);
    CHECK(arena.Seal());
    DXVA_PicParams_H264 params{}; DXVA_Qmatrix_H264 matrix{};
    params.wFrameWidthInMbsMinus1 = 3; params.wFrameHeightInMbsMinus1 = 3; params.num_ref_frames = 2; params.RefPicFlag = 1; params.IntraPicFlag = 1;
    params.MbsConsecutiveFlag = 1; params.MinLumaBipredSize8x8Flag = 1; params.chroma_format_idc = 1; params.frame_mbs_only_flag = 1;
    params.direct_8x8_inference_flag = 1; params.deblocking_filter_control_present_flag = 1; params.Reserved16Bits = 3;
    for (auto& ref : params.RefFrameList) ref.bPicEntry = 255;
    std::memset(&matrix, 16, sizeof(matrix));
    NalSlice slice{}; DecodePicture picture;
    picture.slices = {&slice, 1}; picture.h264 = &params; picture.h264_matrix = &matrix; picture.retain_as_reference = true;
    unsigned compared = 0;
    auto submit = [&](unsigned bytes, std::int64_t pts, SurfaceHandle& handle) {
        slice = {0, bytes};
        picture.packet = {}; picture.packet.pts_100ns = pts; picture.packet.duration_100ns = 1; picture.packet.payload_bytes = bytes;
        picture.packet.payload_block_id = 2; picture.packet.payload_generation = 1; picture.packet.playback_epoch = 1;
        picture.payload = {payload, 0, bytes, {2, 1}};
        CHECK(decoder.Submit(picture, *frames, *returns, &handle) == DecodeStatus::Submitted);
        const auto* frame = frames->PeekReadSlot(); if (!frame) return std::uint64_t{0};
        const auto ready = frame->ready_fence_value;
        std::uint64_t signalled = 0;
        CHECK(SUCCEEDED(verifier.Compare(decoder.SurfaceAt(frame->surface_id), frame->surface_id, decoder.DecodeFence(), ready,
                                         compared++, 0, 64, 64, false, signalled)));
        D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS stats{}; auto state = DecodeStatus::WouldBlock;
        const auto deadline = GetTickCount64() + 10000;
        while (state == DecodeStatus::WouldBlock && GetTickCount64() < deadline) { state = decoder.Statistics(handle, stats); YieldProcessor(); }
        CHECK(state == DecodeStatus::Ready && stats.Status == D3D12_VIDEO_DECODE_STATUS_OK);
        CHECK(decoder.ReturnSurface({handle.id, handle.generation, 1, options.consumer_timeline_id, signalled}));
        (void)frames->ReleaseReadSlot(); (void)returns->ReleaseReadSlot();
        return ready;
    };
    allocation_probe::Begin();
    ProbeBits idr; idr.Ue(0); idr.Ue(2); idr.Ue(0); idr.Value(0, 4); idr.Ue(0); idr.Value(0, 4); idr.Bit(0); idr.Bit(0); idr.Ue(0); idr.Ue(1);
    for (unsigned mb = 0; mb != 16; ++mb) { idr.Ue(25); idr.Pcm(128); }
    SurfaceHandle first{}, second{}, third{};
    (void)submit(idr.Nal(0x65, payload), 0, first);
    // P picture of intra (I_PCM, mb_type 30) macroblocks referencing the IDR.
    ProbeBits intra; intra.Ue(0); intra.Ue(0); intra.Ue(0); intra.Value(1, 4); intra.Value(2, 4); intra.Bit(0); intra.Bit(0); intra.Bit(0); intra.Ue(0); intra.Ue(1);
    for (unsigned mb = 0; mb != 16; ++mb) { intra.Ue(0); intra.Ue(30); intra.Pcm(64); }
    params.IntraPicFlag = 0; params.frame_num = 1; params.CurrFieldOrderCnt[0] = params.CurrFieldOrderCnt[1] = 2;
    params.RefFrameList[0].bPicEntry = 0; params.UsedForReferenceFlags = 3;
    picture.references = {&first, 1};
    const auto p_ready = submit(intra.Nal(0x41, payload), 1, second);
    std::uint64_t signalled = 0;
    CHECK(SUCCEEDED(verifier.Compare(decoder.SurfaceAt(first.id), first.id, decoder.DecodeFence(), p_ready, compared++, 0, 64, 64, false, signalled)));
    // P-skip picture: every macroblock copies the most recent reference (the flat-64 P picture).
    ProbeBits skip; skip.Ue(0); skip.Ue(0); skip.Ue(0); skip.Value(2, 4); skip.Value(4, 4); skip.Bit(0); skip.Bit(0); skip.Bit(0); skip.Ue(0); skip.Ue(1); skip.Ue(16);
    params.frame_num = 2; params.CurrFieldOrderCnt[0] = params.CurrFieldOrderCnt[1] = 4;
    params.FrameNumList[0] = 1; params.FieldOrderCntList[0][0] = params.FieldOrderCntList[0][1] = 2;
    params.RefFrameList[1].bPicEntry = 1; params.FrameNumList[1] = 0; params.FieldOrderCntList[1][0] = params.FieldOrderCntList[1][1] = 0;
    params.UsedForReferenceFlags = 15;
    const SurfaceHandle chain[]{second, first};
    picture.references = {chain, 2}; picture.retain_as_reference = false;
    (void)submit(skip.Nal(0x41, payload), 2, third);
    const auto allocations = allocation_probe::End();
    std::uint32_t sums[8]{};
    CHECK(SUCCEEDED(verifier.Read(compared, sums)));
    const auto flat128 = FlatSum(128), flat64 = FlatSum(64);
    CHECK(sums[0] == flat128); // IDR output.
    CHECK(sums[2] == flat64);  // P picture output.
    CHECK(sums[4] == flat128); // IDR reference unchanged after being referenced.
    CHECK(sums[6] == flat64);  // Motion compensation read the P picture.
    CHECK(allocations == 0);
    std::printf("Decode reference regression: IDR %s, P output %s, reference preserved %s, P-skip copy %s, allocations %llu\n",
                sums[0] == flat128 ? "OK" : "WRONG", sums[2] == flat64 ? "OK" : "WRONG", sums[4] == flat128 ? "OK" : "WRONG",
                sums[6] == flat64 ? "OK" : sums[6] == FlatSum(0) ? "UNWRITTEN" : "WRONG", static_cast<unsigned long long>(allocations));
    CHECK(decoder.ReleaseReference(first)); CHECK(decoder.ReleaseReference(second));
    CHECK(decoder.DrainAndClose() == DecodeStatus::Ready);
    verifier.Destroy();
    return failures > failures_before ? 1 : 0;
}
// Diagnostic (not registered with CTest): prints the syntax the parser derives from the encoder's stream.
int Inspect() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) return 1;
    std::vector<Encoded> stream; bool b_frames = false;
    if (Encode(false, stream, b_frames) != Fixture::Ready) return 77;
    core::LinearMemoryArena arena; (void)arena.Initialize(4 * 1024 * 1024);
    codec::HevcSyntaxParser parser; (void)parser.Initialize(arena);
    unsigned surface = 0;
    for (const auto& e : stream) {
        std::printf("AU pts=%lld bytes=%zu nals:", static_cast<long long>(e.pts), e.au.size());
        ForEachNal(e.au, [&](std::span<const std::byte> nal) {
            std::printf(" %u(%zu)", (std::to_integer<unsigned>(nal[0]) >> 1) & 63, nal.size());
        });
        const auto status = parser.Parse({e.au.data(), e.au.size()}, 4);
        std::printf(" -> status %u\n", static_cast<unsigned>(status));
        const auto* p = parser.Picture(); if (!p) continue;
        if (!surface) {
            const auto& s = *p->sps; const auto& q = *p->pps;
            std::printf(" SPS %ux%u crop %u,%u-%u,%u ctb=%u mincb=%u tb=%u+%u depth=%u/%u dec=%u reorder=%u poc_bits=%u rps=%u lt=%u/%u tmvp=%u sis=%u scaling=%u amp=%u sao=%u pcm=%u sub=%u\n",
                s.width, s.height, s.left, s.top, s.right, s.bottom, s.log2_min_cb_minus3 + 3u + s.log2_diff_max_min_cb, s.log2_min_cb_minus3 + 3u,
                s.log2_min_tb_minus2 + 2u, s.log2_diff_max_min_tb, s.max_transform_depth_inter, s.max_transform_depth_intra,
                s.max_dec_pic_buffering_minus1, s.max_num_reorder, s.poc_lsb_bits, s.num_short_term_rps, s.long_term_present, s.num_long_term_sps,
                s.temporal_mvp, s.strong_intra_smoothing, s.scaling_list_enabled, s.amp, s.sao, s.pcm, s.max_sub_layers_minus1);
            for (unsigned i = 0; i < s.num_short_term_rps; ++i) {
                std::printf("  sps rps[%u]: neg", i);
                for (unsigned j = 0; j < s.rps[i].negative; ++j) std::printf(" %d%s", s.rps[i].delta_s0[j], (s.rps[i].used_s0 >> j) & 1 ? "u" : "");
                std::printf(" pos");
                for (unsigned j = 0; j < s.rps[i].positive; ++j) std::printf(" %d%s", s.rps[i].delta_s1[j], (s.rps[i].used_s1 >> j) & 1 ? "u" : "");
                std::printf(" refdeltas=%u\n", s.rps[i].reference_delta_pocs);
            }
            std::printf(" PPS dep=%u outflag=%u extra=%u sdh=%u cabacinit=%u l0=%u l1=%u qp=%d cuqp=%u/%u cb=%d cr=%d sliceqp=%u wp=%u wbp=%u tqb=%u tiles=%u wpp=%u lfs=%u dbo=%u dbd=%u beta=%d tc=%d lm=%u merge=%u she=%u tskip=%u cip=%u\n",
                q.dependent_slices, q.output_flag_present, q.extra_slice_header_bits, q.sign_data_hiding, q.cabac_init_present,
                q.refs_l0_minus1, q.refs_l1_minus1, q.init_qp_minus26, q.cu_qp_delta, q.diff_cu_qp_delta_depth, q.cb_qp_offset, q.cr_qp_offset,
                q.slice_chroma_qp_offsets, q.weighted_pred, q.weighted_bipred, q.transquant_bypass, q.tiles, q.entropy_sync,
                q.loop_filter_across_slices, q.deblocking_override, q.deblocking_disabled, q.beta_offset_div2, q.tc_offset_div2,
                q.lists_modification, q.log2_parallel_merge_minus2, q.slice_header_extension, q.transform_skip, q.constrained_intra);
        }
        std::printf(" pic nal=%u tid=%u poc=%d slices=%u rpsbits=%u refdeltas=%u intra=%u retain=%u refs:",
                    p->nal_type, p->temporal_id, p->poc, p->slice_count, p->short_term_rps_bits, p->reference_rps_delta_pocs, p->intra, p->retain);
        for (unsigned i = 0; i < p->reference_count; ++i) std::printf(" %d%s", p->references[i].poc, p->references[i].long_term ? "L" : "");
        std::printf(" before:"); for (unsigned i = 0; i < p->st_curr_before_count; ++i) std::printf(" %u", p->st_curr_before[i]);
        std::printf(" after:"); for (unsigned i = 0; i < p->st_curr_after_count; ++i) std::printf(" %u", p->st_curr_after[i]);
        std::printf(" lt:"); for (unsigned i = 0; i < p->lt_curr_count; ++i) std::printf(" %u", p->lt_curr[i]);
        std::printf("\n");
        SurfaceHandle evicted[16]{}; unsigned count = 0; (void)parser.Commit({surface++, 1}, evicted, count);
    }
    (void)MFShutdown(); CoUninitialize();
    return 0;
}
ID3D12Device* HardwareDevice() {
    ID3D12Debug* debug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); Release(debug); }
    IDXGIFactory6* factory = nullptr; ID3D12Device* device = nullptr;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        for (UINT i = 0; !device; ++i) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                (void)D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            Release(adapter);
        }
    }
    Release(factory);
    if (!device) std::puts("SKIP no hardware D3D12 device");
    return device;
}
void ReportValidation(ID3D12Device* device) {
    ID3D12InfoQueue* messages = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&messages)))) return;
    const auto count = messages->GetNumStoredMessages();
    alignas(D3D12_MESSAGE) std::array<std::byte, 4096> storage{};
    for (UINT64 i = 0; i != count; ++i) {
        SIZE_T size = storage.size(); auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(messages->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::printf("D3D12 validation error: %s\n", message->pDescription); ++failures;
        }
    }
    Release(messages);
}
// ---- Hardware: H.264 reference lists, pixel-exact ----
// Handwritten 64x64 Main-profile streams through the full path (H264SyntaxParser, H264Dxva, D3D12VideoDecoder).
// Reference pictures are flat I_PCM planes of distinct luma. A fully skipped P picture copies RefPicList0[0]; a fully
// skipped B picture (spatial direct, no neighbours with motion, default weights) is exactly (L0[0] + L1[0] + 1) / 2.
// Every output luma therefore follows from the DPB marking, long-term state and POCs the host passed: any error in
// them changes the lists the accelerator builds, and so the pixels.
using Unit = std::vector<std::byte>;
struct ListOp { unsigned op, value, index; };
struct ListPicture {
    char kind = 'I';            // 'I' I_PCM, 'P' P_Skip, 'B' B_Skip.
    bool idr = false, reference = true;
    unsigned frame = 0, lsb = 0, luma = 0, expect = 0, idr_id = 0;
    ListOp modification[2][2]{}; unsigned modifications[2]{};
    bool long_term = false; ListOp mmco[4]{}; unsigned mmco_count = 0;
};
struct AvcBits : ProbeBits { void Se(int v) { Ue(static_cast<unsigned>(v <= 0 ? -2 * v : 2 * v - 1)); } };
void AppendNal(Unit& au, AvcBits& b, unsigned header) {
    std::vector<std::byte> nal(16384); const auto n = b.Nal(header, nal.data());
    for (unsigned i = 4; i-- > 0;) au.push_back(static_cast<std::byte>((n >> (8 * i)) & 255));
    au.insert(au.end(), nal.begin(), nal.begin() + n);
}
Unit ListUnit(const ListPicture& p, unsigned poc, bool parameter_sets) {
    Unit au;
    if (parameter_sets) {
        AvcBits s; s.Value(77, 8); s.Value(0, 8); s.Value(30, 8); s.Ue(0); s.Ue(0); s.Ue(poc); // Main 3.0, 16 frame numbers.
        if (poc == 0) s.Ue(4);                                                                // 8-bit POC lsb.
        if (poc == 1) { s.Bit(1); s.Se(-3); s.Se(0); s.Ue(1); s.Se(2); }                     // Cycle {2}, non-reference -3.
        s.Ue(4); s.Bit(0); s.Ue(3); s.Ue(3); s.Bit(1); s.Bit(1); s.Bit(0); s.Bit(0);         // 4 refs, 64x64, direct 8x8.
        AppendNal(au, s, 0x67);
        AvcBits q; q.Ue(0); q.Ue(0); q.Bit(0); q.Bit(0); q.Ue(0); q.Ue(0); q.Ue(0); q.Bit(0); q.Value(0, 2);
        q.Ue(0); q.Ue(0); q.Ue(0); q.Bit(1); q.Bit(0); q.Bit(0);                              // CAVLC, deblocking control.
        AppendNal(au, q, 0x68);
    }
    AvcBits b; b.Ue(0); b.Ue(p.kind == 'P' ? 0 : p.kind == 'B' ? 1 : 2); b.Ue(0); b.Value(p.frame, 4);
    if (p.idr) b.Ue(p.idr_id);
    if (poc == 0) b.Value(p.lsb, 8);
    if (p.kind == 'B') b.Bit(1);   // direct_spatial_mv_pred_flag
    if (p.kind != 'I') b.Bit(0);   // num_ref_idx_active_override_flag: one entry per list.
    for (unsigned list = 0; list < (p.kind == 'B' ? 2u : p.kind == 'P' ? 1u : 0u); ++list) {
        b.Bit(p.modifications[list] != 0);
        if (!p.modifications[list]) continue;
        for (unsigned i = 0; i < p.modifications[list]; ++i) { b.Ue(p.modification[list][i].op); b.Ue(p.modification[list][i].value); }
        b.Ue(3);
    }
    if (p.reference) {
        if (p.idr) { b.Bit(0); b.Bit(p.long_term); }
        else {
            b.Bit(p.mmco_count != 0);
            for (unsigned i = 0; i < p.mmco_count; ++i) {
                const auto& m = p.mmco[i]; b.Ue(m.op);
                if (m.op >= 1 && m.op <= 4) b.Ue(m.value);
                if (m.op == 3 || m.op == 6) b.Ue(m.index);
            }
            if (p.mmco_count) b.Ue(0);
        }
    }
    b.Ue(0); b.Ue(1); // slice_qp_delta 0, deblocking disabled: flat planes stay exact.
    if (p.kind == 'I') for (unsigned mb = 0; mb != 16; ++mb) { b.Ue(25); b.Pcm(p.luma); }
    else b.Ue(16); // mb_skip_run over the whole picture.
    AppendNal(au, b, (p.idr ? 5u : 1u) | (p.reference ? 0x60u : 0u));
    return au;
}
int ListRun(ID3D12Device* device, const char* label, unsigned poc, const std::vector<ListPicture>& pictures) {
    const int failures_before = failures;
    std::vector<Unit> units;
    for (unsigned i = 0; i < pictures.size(); ++i) units.push_back(ListUnit(pictures[i], poc, i == 0));
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
    codec::H264SyntaxParser parser; CHECK(parser.Initialize(arena));
    auto* dxva = arena.Create<codec::H264Dxva>();
    auto* frames = arena.Create<core::SpscRingBuffer<core::VideoFrameDescriptor, 16>>();
    auto* returns = arena.Create<core::SpscRingBuffer<core::MediaPacket, 16>>();
    auto* payload = static_cast<std::byte*>(arena.Allocate(32768));
    if (!dxva || !frames || !returns || !payload) return 1;
    Verifier verifier{};
    if (FAILED(verifier.Create(device))) { verifier.Destroy(); return 1; }
    D3D12VideoDecoder decoder; DecoderOptions options;
    options.width = options.height = 64; options.surfaces = 10; options.max_bitstream_bytes = 32768;
    const auto prepared = decoder.Prepare(arena, device, verifier.fence, options);
    if (prepared != DecodeStatus::Ready) {
        std::printf("%s H.264 decoder prepare: %08lx\n", prepared == DecodeStatus::Unsupported ? "SKIP" : "FAIL", static_cast<unsigned long>(decoder.LastError()));
        verifier.Destroy(); return prepared == DecodeStatus::Unsupported ? 77 : 1;
    }
    verifier.Bind(device, decoder, options.surfaces, false);
    CHECK(arena.Seal());
    std::int32_t pocs[48]{}; unsigned compared = 0, bad_status = 0;
    allocation_probe::Begin();
    for (unsigned i = 0; i < units.size() && i < 48; ++i) {
        std::memcpy(payload, units[i].data(), units[i].size());
        const std::span<const std::byte> bytes{payload, units[i].size()};
        const auto status = parser.Parse(bytes, 4);
        CHECK(status == codec::SyntaxStatus::Ready);
        if (status != codec::SyntaxStatus::Ready) break;
        const auto* picture = parser.Picture(); pocs[i] = picture->top_poc;
        core::MediaPacket packet{};
        packet.pts_100ns = i; packet.duration_100ns = 1; packet.payload_bytes = static_cast<std::uint32_t>(units[i].size());
        packet.payload_block_id = 2; packet.payload_generation = 1; packet.playback_epoch = 1;
        const auto native = dxva->Prepare(*picture, packet, {payload, 0, packet.payload_bytes, {2, 1}});
        SurfaceHandle surface{};
        CHECK(decoder.Submit(native, *frames, *returns, &surface) == DecodeStatus::Submitted);
        while (returns->PeekReadSlot()) (void)returns->ReleaseReadSlot();
        if (const auto* frame = frames->PeekReadSlot()) {
            std::uint64_t signalled = 0;
            CHECK(SUCCEEDED(verifier.Compare(decoder.SurfaceAt(frame->surface_id), frame->surface_id, decoder.DecodeFence(),
                                             frame->ready_fence_value, compared++, 0, 64, 64, false, signalled)));
            D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS stats{}; auto state = DecodeStatus::WouldBlock;
            const auto deadline = GetTickCount64() + 10000;
            while (state == DecodeStatus::WouldBlock && GetTickCount64() < deadline) { state = decoder.Statistics(surface, stats); YieldProcessor(); }
            if (state != DecodeStatus::Ready || stats.Status != D3D12_VIDEO_DECODE_STATUS_OK) ++bad_status;
            CHECK(decoder.ReturnSurface({frame->surface_id, frame->surface_generation, 1, options.consumer_timeline_id, signalled}));
            (void)frames->ReleaseReadSlot();
        }
        SurfaceHandle evicted[16]{}; unsigned count = 0;
        CHECK(parser.Commit(surface, evicted, count));
        for (unsigned k = 0; k < count; ++k) CHECK(decoder.ReleaseReference(evicted[k]));
    }
    SurfaceHandle released[16]{}; unsigned count = 0;
    CHECK(parser.Reset(released, count));
    for (unsigned k = 0; k < count; ++k) CHECK(decoder.ReleaseReference(released[k]));
    const auto allocations = allocation_probe::End();
    std::uint32_t sums[96]{};
    CHECK(compared == pictures.size() && SUCCEEDED(verifier.Read(compared, sums)));
    unsigned exact = 0;
    for (unsigned i = 0; i < compared; ++i) {
        const auto& p = pictures[i];
        const bool good = sums[2 * i] == FlatSum(p.expect);
        exact += good ? 1u : 0u;
        if (!good) {
            // The nearest flat value, for the diagnosis.
            unsigned nearest = 0; for (unsigned v = 1; v < 256; ++v) if (FlatSum(v) < FlatSum(nearest)) nearest = v;
            unsigned seen = 0; for (unsigned v = 0; v < 256; ++v) if (FlatSum(v) == sums[2 * i]) seen = v;
            std::printf("FAIL %s picture %u (%c, POC %d): expected luma %u, decoded %s%u\n", label, i, p.kind, pocs[i], p.expect,
                        seen ? "" : "non-flat, sum ", seen ? seen : sums[2 * i]);
            ++failures; (void)nearest;
        }
    }
    CHECK(bad_status == 0 && allocations == 0);
    std::printf("H.264 reference lists (%s): %u/%zu pictures exact, decode status OK=%s, allocations %llu\n", label, exact, pictures.size(),
                bad_status ? "no" : "yes", static_cast<unsigned long long>(allocations));
    CHECK(decoder.DrainAndClose() == DecodeStatus::Ready);
    verifier.Destroy();
    return failures > failures_before ? 1 : 0;
}
int H264Lists() {
    auto* device = HardwareDevice();
    if (!device) return 77;
    using P = ListPicture;
    auto intra = [](unsigned frame, unsigned lsb, unsigned luma) { P p; p.frame = frame; p.lsb = lsb; p.luma = p.expect = luma; return p; };
    auto skip = [](char kind, unsigned frame, unsigned lsb, unsigned expect) { P p; p.kind = kind; p.reference = false; p.frame = frame; p.lsb = lsb; p.expect = expect; return p; };
    std::vector<P> marking;
    { P p = intra(0, 0, 40); p.idr = true; marking.push_back(p); }            // A  POC 0
    marking.push_back(intra(1, 8, 80));                                       // B  POC 8
    marking.push_back(intra(2, 16, 120));                                     // C  POC 16
    marking.push_back(intra(3, 24, 160));                                     // D  POC 24
    marking.push_back(skip('P', 4, 26, 160));                                 // Default L0[0]: highest PicNum (D).
    { P p = skip('P', 4, 27, 80); p.modification[0][0] = {0, 2, 0}; p.modifications[0] = 1; marking.push_back(p); } // PicNum 1 (B).
    marking.push_back(skip('B', 4, 12, 100));                                 // L0[0] B (POC 8), L1[0] C (POC 16).
    { P p = skip('B', 4, 13, 60); p.modification[1][0] = {0, 3, 0}; p.modifications[1] = 1; marking.push_back(p); } // L1[0] = A.
    { P p = intra(4, 40, 200); p.mmco[0] = {1, 2, 0}; p.mmco_count = 1; marking.push_back(p); }                  // E; MMCO 1 drops B.
    marking.push_back(skip('B', 5, 12, 80));                                  // L0[0] is now A: (40 + 120) / 2.
    { P p = intra(5, 48, 220); p.mmco[0] = {1, 4, 0}; p.mmco[1] = {4, 2, 0}; p.mmco[2] = {3, 1, 0}; p.mmco_count = 3; marking.push_back(p); }
                                                                              // F; drops A, D becomes long-term 0.
    { P p = skip('P', 6, 49, 160); p.modification[0][0] = {2, 0, 0}; p.modifications[0] = 1; marking.push_back(p); } // LongTermPicNum 0.
    marking.push_back(skip('P', 6, 50, 220));                                 // Short-term PicNum order: F first.
    { P p = intra(6, 56, 30); p.mmco[0] = {2, 0, 0}; p.mmco[1] = {6, 0, 1}; p.mmco_count = 2; marking.push_back(p); }
                                                                              // G; drops D, becomes long-term 1.
    { P p = skip('P', 7, 57, 30); p.modification[0][0] = {2, 1, 0}; p.modifications[0] = 1; marking.push_back(p); } // LongTermPicNum 1.
    marking.push_back(skip('B', 7, 44, 210));                                 // L0[0] E (POC 40), L1[0] F (POC 48); G long-term last.
    { P p = intra(7, 64, 90); p.mmco[0] = {5, 0, 0}; p.mmco_count = 1; marking.push_back(p); }                   // H; MMCO 5.
    marking.push_back(skip('B', 1, 2, 90));                                   // Only H remains (frame_num 0, POC 0).
    marking.push_back(skip('P', 1, 3, 90));
    { P p = intra(0, 0, 70); p.idr = true; p.idr_id = 1; p.long_term = true; marking.push_back(p); } // IDR, long-term 0.
    { P p = skip('P', 1, 2, 70); p.modification[0][0] = {2, 0, 0}; p.modifications[0] = 1; marking.push_back(p); }
    marking.push_back(skip('P', 1, 3, 70));                                   // Only a long-term reference: still L0[0].
    // POC type 1 (cycle {2}, non-reference offset -3): the non-reference B has POC 4 - 3 = 1, between A (0) and B (2).
    std::vector<P> type1;
    { P p = intra(0, 0, 40); p.idr = true; type1.push_back(p); }
    type1.push_back(intra(1, 0, 80)); type1.push_back(intra(2, 0, 120));
    type1.push_back(skip('B', 3, 0, 60)); type1.push_back(skip('P', 3, 0, 120));
    // POC type 2: the non-reference B has POC 5, after every reference; L1 then equals L0 and its first two entries swap.
    std::vector<P> type2;
    { P p = intra(0, 0, 40); p.idr = true; type2.push_back(p); }
    type2.push_back(intra(1, 0, 80)); type2.push_back(intra(2, 0, 120));
    type2.push_back(skip('B', 3, 0, 100)); type2.push_back(skip('P', 3, 0, 120));
    int result = ListRun(device, "MMCO 1-6, long-term, modification", 0, marking);
    if (result != 77) result = ListRun(device, "POC type 1", 1, type1) | result;
    if (result != 77) result = ListRun(device, "POC type 2", 2, type2) | result;
    ReportValidation(device); Release(device);
    return failures ? 1 : result;
}
// ---- Hardware: H.264 B-frame streams from the platform encoders ----
// Presentation order comes from the POC (rank inside each IDR period, closed GOPs) and is cross-checked against the
// encoder's own sample times. Every picture is compared with its source frame, and the output order is replayed
// through the renderer's ReorderWindow at the SPS reorder depth.
int H264DecodeRun(ID3D12Device* device, const std::vector<Encoded>& stream, const char* label) {
    const int failures_before = failures;
    const unsigned n = static_cast<unsigned>((std::min)(stream.size(), std::size_t{kFrames}));
    // Setup pass (unmeasured): POCs through a scratch parser, then display order and the encoder's timestamp order.
    std::vector<int> poc(n); std::vector<unsigned> display(n); std::vector<bool> idr(n);
    unsigned b_pictures = 0, reference_b = 0, adaptive = 0, reorder = 0;
    {
        core::LinearMemoryArena scratch; codec::H264SyntaxParser parser;
        if (!scratch.Initialize(4 * 1024 * 1024) || !parser.Initialize(scratch)) return 1;
        for (unsigned i = 0; i < n; ++i) {
            const auto status = parser.Parse({stream[i].au.data(), stream[i].au.size()}, 4);
            CHECK(status == codec::SyntaxStatus::Ready);
            if (status != codec::SyntaxStatus::Ready) return 1;
            const auto* p = parser.Picture();
            poc[i] = p->top_poc; idr[i] = p->idr;
            b_pictures += p->bidirectional ? 1u : 0u; reference_b += p->bidirectional && p->reference ? 1u : 0u;
            adaptive += p->adaptive ? 1u : 0u; reorder = p->sps->reorder;
            SurfaceHandle evicted[16]{}; unsigned count = 0; CHECK(parser.Commit({i, 1}, evicted, count));
        }
    }
    unsigned timestamp_errors = 0;
    for (unsigned start = 0; start < n;) {
        unsigned end = start + 1; while (end < n && !idr[end]) ++end;
        for (unsigned i = start; i < end; ++i) {
            unsigned by_poc = 0, by_time = 0;
            for (unsigned j = start; j < end; ++j) { by_poc += poc[j] < poc[i] ? 1u : 0u; by_time += stream[j].pts < stream[i].pts ? 1u : 0u; }
            display[i] = start + by_poc;
            timestamp_errors += by_poc != by_time ? 1u : 0u;
        }
        start = end;
    }
    CHECK(timestamp_errors == 0);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(16 * 1024 * 1024));
    codec::H264SyntaxParser parser; CHECK(parser.Initialize(arena));
    auto* dxva = arena.Create<codec::H264Dxva>();
    auto* frames = arena.Create<core::SpscRingBuffer<core::VideoFrameDescriptor, 16>>();
    auto* returns = arena.Create<core::SpscRingBuffer<core::MediaPacket, 16>>();
    auto* window = arena.Create<video::ReorderWindow<16>>();
    std::size_t largest = 0; for (unsigned i = 0; i < n; ++i) largest = (std::max)(largest, stream[i].au.size());
    const auto capacity = (largest + 127) & ~std::size_t{127};
    auto* payload = static_cast<std::byte*>(arena.Allocate(capacity));
    if (!dxva || !frames || !returns || !window || !payload) return 1;
    CHECK(window->Configure(reorder));
    Verifier verifier{};
    if (FAILED(verifier.Create(device))) { verifier.Destroy(); return 1; }
    D3D12VideoDecoder decoder; DecoderOptions options;
    options.width = kWidth; options.height = kHeight; options.surfaces = 16;
    options.max_bitstream_bytes = static_cast<std::uint32_t>((capacity + 3 * 256 + 127) & ~std::size_t{127});
    const auto prepared = decoder.Prepare(arena, device, verifier.fence, options);
    if (prepared != DecodeStatus::Ready) {
        std::printf("%s H.264 decoder prepare: %08lx\n", prepared == DecodeStatus::Unsupported ? "SKIP" : "FAIL", static_cast<unsigned long>(decoder.LastError()));
        verifier.Destroy(); return prepared == DecodeStatus::Unsupported ? 77 : 1;
    }
    verifier.Bind(device, decoder, options.surfaces, false);
    CHECK(arena.Seal());
    const auto arena_allocations = arena.Statistics().allocation_count;
    unsigned processed = 0, bad_status = 0, seen[kFrames]{}, order_errors = 0, late = 0, presented = 0;
    std::int64_t last_presented = -kFrameDuration;
    auto present = [&](bool draining) {
        while (window->Ready(draining)) {
            const auto pts = window->Front()->pts;
            if (pts != last_presented + kFrameDuration) ++order_errors; // Strictly the next frame of the timeline.
            last_presented = pts; ++presented; (void)window->Pop();
        }
    };
    auto consume = [&]() -> bool {
        const auto* frame = frames->PeekReadSlot();
        if (!frame) return false;
        const SurfaceHandle handle{frame->surface_id, frame->surface_generation};
        const auto index = static_cast<unsigned>((frame->pts_100ns + kFrameDuration / 2) / kFrameDuration);
        if (index < kFrames) ++seen[index];
        if (window->Insert(frame->pts_100ns, handle.id) != video::ReorderInsert::Inserted) ++late;
        present(false);
        std::uint64_t signalled = 0;
        CHECK(SUCCEEDED(verifier.Compare(decoder.SurfaceAt(frame->surface_id), frame->surface_id, decoder.DecodeFence(),
                                         frame->ready_fence_value, processed, index, kWidth, kHeight, false, signalled)));
        D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS stats{}; auto state = DecodeStatus::WouldBlock;
        const auto deadline = GetTickCount64() + 10000;
        while (state == DecodeStatus::WouldBlock && GetTickCount64() < deadline) { state = decoder.Statistics(handle, stats); YieldProcessor(); }
        if (state != DecodeStatus::Ready || stats.Status != D3D12_VIDEO_DECODE_STATUS_OK || stats.NumMacroblocksAffected) ++bad_status;
        CHECK(decoder.ReturnSurface({handle.id, handle.generation, frame->playback_epoch, options.consumer_timeline_id, signalled}));
        (void)frames->ReleaseReadSlot(); ++processed; return true;
    };
    allocation_probe::Begin();
    for (unsigned i = 0; i < n; ++i) {
        std::memcpy(payload, stream[i].au.data(), stream[i].au.size());
        const auto status = parser.Parse({payload, stream[i].au.size()}, 4);
        CHECK(status == codec::SyntaxStatus::Ready);
        if (status != codec::SyntaxStatus::Ready) break;
        const auto* picture = parser.Picture();
        core::MediaPacket packet{};
        packet.pts_100ns = static_cast<std::int64_t>(display[i]) * kFrameDuration; packet.dts_100ns = i;
        packet.duration_100ns = kFrameDuration; packet.payload_bytes = static_cast<std::uint32_t>(stream[i].au.size());
        packet.payload_block_id = 2; packet.payload_generation = 1; packet.playback_epoch = 1;
        const auto native = dxva->Prepare(*picture, packet, {payload, 0, packet.payload_bytes, {2, 1}});
        SurfaceHandle surface{}; auto decoded = DecodeStatus::WouldBlock;
        for (unsigned spin = 0; spin < 1000000 && decoded == DecodeStatus::WouldBlock; ++spin) {
            decoded = decoder.Submit(native, *frames, *returns, &surface);
            if (decoded == DecodeStatus::WouldBlock && !consume()) YieldProcessor();
        }
        CHECK(decoded == DecodeStatus::Submitted);
        if (decoded != DecodeStatus::Submitted) { parser.Cancel(); break; }
        while (returns->PeekReadSlot()) (void)returns->ReleaseReadSlot();
        SurfaceHandle evicted[16]{}; unsigned count = 0;
        CHECK(parser.Commit(surface, evicted, count));
        for (unsigned k = 0; k < count; ++k) CHECK(decoder.ReleaseReference(evicted[k]));
        while (consume()) {}
    }
    while (consume()) {}
    present(true);
    SurfaceHandle released[16]{}; unsigned count = 0;
    CHECK(parser.Reset(released, count));
    for (unsigned k = 0; k < count; ++k) CHECK(decoder.ReleaseReference(released[k]));
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0 && arena.Statistics().allocation_count == arena_allocations);
    CHECK(processed == n && bad_status == 0 && presented == n && order_errors == 0 && late == 0);
    bool complete = true; for (unsigned i = 0; i < n; ++i) complete = complete && seen[i] == 1;
    CHECK(complete);
    std::uint32_t sums[kFrames * 2]{};
    const auto read = verifier.Read(processed, sums);
    CHECK(SUCCEEDED(read));
    double worst = 0, weakest_control = 1e9;
    const double pixels = static_cast<double>(kWidth) * kHeight;
    for (unsigned i = 0; SUCCEEDED(read) && i < processed; ++i) {
        const double error = sums[2 * i] / pixels, control = sums[2 * i + 1] / pixels;
        if (error >= 4.0 || control <= 8.0) std::printf("FAIL %s output %u: error %.3f, control %.3f\n", label, i, error, control);
        CHECK(error < 4.0 && control > 8.0);
        worst = (std::max)(worst, error); weakest_control = (std::min)(weakest_control, control);
    }
    std::printf("H.264 %s: %u pictures (%u B, %u reference B, %u with MMCO), reorder depth %u, presentation order %s "
                "(timestamp cross-check %u errors), decode status OK=%s, max mean |luma error| %.3f (control >= %.3f), allocations %llu\n",
                label, processed, b_pictures, reference_b, adaptive, reorder, order_errors ? "WRONG" : "exact", timestamp_errors,
                bad_status ? "no" : "yes", worst, weakest_control, static_cast<unsigned long long>(allocations));
    CHECK(b_pictures > 0);
    CHECK(decoder.DrainAndClose() == DecodeStatus::Ready);
    verifier.Destroy();
    return failures > failures_before ? 1 : 0;
}
int H264Decode() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) { CoUninitialize(); return 1; }
    int result = 77;
    if (auto* device = HardwareDevice()) {
        struct Variant { const char* label; bool software, main; unsigned b_frames; };
        const Variant variants[]{{"hardware encoder, B-pyramid (3 B)", false, false, 3}, {"hardware encoder, IBBP", false, false, 2},
                                 {"software encoder, High CAVLC IBBP", true, false, 2}, {"software encoder, Main IBBP", true, true, 2}};
        unsigned passed = 0;
        for (const auto& v : variants) {
            encoder::Options o; o.h264 = true; o.software = v.software; o.main = v.main; o.b_frames = v.b_frames;
            o.width = kWidth; o.height = kHeight; o.frames = kFrames; o.fps = 30; o.gop = 24; o.bitrate = 4000000;
            std::vector<Encoded> stream; bool b_frames = false;
            const auto fixture = encoder::Encode(o, FillRow, stream, b_frames);
            if (fixture == Fixture::Unavailable) { std::printf("SKIP %s: unavailable\n", v.label); continue; }
            if (fixture == Fixture::Failed) { ++failures; continue; }
            if (H264DecodeRun(device, stream, v.label) == 0) ++passed;
        }
        ReportValidation(device); Release(device);
        result = failures ? 1 : passed ? 0 : 77;
    }
    (void)MFShutdown(); CoUninitialize();
    return result;
}
int Reference() {
    auto* device = HardwareDevice();
    if (!device) return 77;
    const auto result = ReferenceRun(device);
    ReportValidation(device); Release(device);
    return failures ? 1 : result;
}
int Decode() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(mf)) { std::printf("FAIL MFStartup: %08lx\n", static_cast<unsigned long>(mf)); CoUninitialize(); return 1; }
    int result = 77;
    if (auto* device = HardwareDevice()) {
        unsigned passed = 0;
        for (const bool ten_bit : {false, true}) {
            std::vector<Encoded> stream; bool b_frames = false;
            const auto fixture = Encode(ten_bit, stream, b_frames);
            if (fixture == Fixture::Unavailable) { std::printf("SKIP %s: no platform HEVC encoder accepts this format\n", ten_bit ? "Main10" : "Main"); continue; }
            if (fixture == Fixture::Failed) { ++failures; continue; }
            std::printf("HEVC %s fixture: %zu access units from the platform encoder (B pictures requested: %s)\n",
                        ten_bit ? "Main10" : "Main", stream.size(), b_frames ? "accepted" : "refused");
            if (DecodeRun(device, stream, ten_bit) == 0) ++passed;
        }
        ReportValidation(device); Release(device);
        result = failures ? 1 : passed ? 0 : 77;
    }
    (void)MFShutdown(); CoUninitialize();
    return result;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "dxva") == 0) return Dxva();
    if (argc == 2 && std::strcmp(argv[1], "reference") == 0) return Reference();
    if (argc == 2 && std::strcmp(argv[1], "decode") == 0) return Decode();
    if (argc == 2 && std::strcmp(argv[1], "inspect") == 0) return Inspect();
    if (argc == 2 && std::strcmp(argv[1], "h264-lists") == 0) return H264Lists();
    if (argc == 2 && std::strcmp(argv[1], "h264-decode") == 0) return H264Decode();
    return 1;
}
