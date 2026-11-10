#include "media/render/NeuralUpscalePass.hpp"
#include "NeuralShader.hpp"
#include <cstring>
#include <utility>

namespace media::render {
namespace {
template <class T> void Drop(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
// Constant-buffer layout of NeuralSuperRes.hlsl: the model as declared (1356 floats).
constexpr unsigned kPacked = sizeof(video::NeuralModel) / sizeof(float);
constexpr UINT64 kWeightBytes = (sizeof(video::NeuralModel) + 255) & ~UINT64{255}; // Constant buffers: 256-byte multiples.
void Pack(const video::NeuralModel& m, float (&packed)[kPacked]) noexcept { std::memcpy(packed, &m, sizeof(m)); }
constexpr UINT kConstants = 8; // NeuralSuperRes.hlsl's Region.
constexpr unsigned kKeepAnchors = 1, kReuse = 2, kTolerant = 4;
constexpr unsigned kAnchorBytes = 20 * 20 * 2; // A tile's 20x20 codes, 16 bits each.
constexpr UINT64 kStatsBytes = 16;             // Reused and evaluated tiles (cumulative), 8 spare bytes.
}
HRESULT NeuralUpscalePass::Create(ID3D12Device* device, ID3D12CommandQueue* queue, const video::NeuralModel& model, unsigned max_width,
                                  unsigned max_height) noexcept {
    if (!device || !queue || root_ || !max_width || !max_height) return E_INVALIDARG;
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
    D3D12_ROOT_PARAMETER parameters[5]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 0, kConstants};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[1].Descriptor = {1, 0};
    for (unsigned i = 0; i != 2; ++i) { parameters[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[2 + i].DescriptorTable = {1, &ranges[i]}; }
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[4].Descriptor = {1, 0}; // u1: the history.
    const D3D12_ROOT_SIGNATURE_DESC signature{5, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    Drop(blob); Drop(errors);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root_; pso.CS = {g_NeuralShader, sizeof(g_NeuralShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_));
    // Weights: default heap, filled once through an upload buffer.
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = kWeightBytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr; ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr; ID3D12Fence* fence = nullptr;
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&weights_));
    // Anchors: every tile of the largest crop, 800 bytes each (1080p: 8160 tiles, 6.5 MB), then the tile counters. An
    // anchor is never read before its tile's first evaluation has written it (the first Record after Create or Invalidate
    // may not reuse); the counters start at zero.
    anchor_bytes_ = (max_width + 15) / 16 * ((max_height + 15) / 16) * kAnchorBytes;
    D3D12_RESOURCE_DESC codes = buffer; codes.Width = anchor_bytes_ + kStatsBytes; codes.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &codes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&history_));
    history_width_ = max_width; history_height_ = max_height; valid_ = false; records_ = reuse_records_ = 0;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC counters = buffer; counters.Width = kStatsBytes;
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &counters, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_));
    void* counted = nullptr;
    if (SUCCEEDED(hr)) hr = readback_->Map(0, nullptr, &counted); // Mapped for the pass's life.
    counters_ = static_cast<const std::uint32_t*>(counted);
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC staging = buffer; staging.Width = kWeightBytes + kStatsBytes; // The weights, then zeros for the counters.
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &staging, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    void* mapped = nullptr; const D3D12_RANGE none{0, 0};
    if (SUCCEEDED(hr)) hr = upload->Map(0, &none, &mapped);
    if (SUCCEEDED(hr)) {
        float packed[kPacked]; Pack(model, packed); // Setup only (5.3 KB on the stack).
        std::memset(mapped, 0, kWeightBytes + kStatsBytes); std::memcpy(mapped, packed, sizeof(packed)); upload->Unmap(0, nullptr);
    }
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        list->CopyBufferRegion(weights_, 0, upload, 0, kWeightBytes);
        list->CopyBufferRegion(history_, anchor_bytes_, upload, kWeightBytes, kStatsBytes);
        D3D12_RESOURCE_BARRIER barriers[2]{}; barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition = {weights_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER};
        barriers[1].Transition = {history_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON};
        list->ResourceBarrier(2, barriers);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = event ? queue->Signal(fence, 1) : HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, event);
        // Setup only: the upload buffer must outlive the copy.
        if (SUCCEEDED(hr) && WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) hr = E_FAIL;
        if (event) CloseHandle(event);
    }
    Drop(fence); Drop(list); Drop(allocator); Drop(upload);
    if (FAILED(hr)) Release();
    return hr;
}
void NeuralUpscalePass::Record(ID3D12GraphicsCommandList* list, D3D12_GPU_DESCRIPTOR_HANDLE luma, D3D12_GPU_DESCRIPTOR_HANDLE output,
                               unsigned left, unsigned top, unsigned right, unsigned bottom, bool ten_bit) noexcept {
    // Reuse needs anchors of the same crop and format, written by Records whose output this one writes again; a crop larger
    // than the history keeps none.
    const bool fits = right - left <= history_width_ && bottom - top <= history_height_;
    const bool same = valid_ && last_[0] == left && last_[1] == top && last_[2] == right && last_[3] == bottom && last_ten_bit_ == ten_bit;
    const bool reuse = same && reuse_ != TemporalReuse::Off;
    const unsigned temporal = fits ? kKeepAnchors | (reuse ? kReuse : 0u) | (reuse && reuse_ == TemporalReuse::Tolerant ? kTolerant : 0u) : 0u;
    const unsigned constants[kConstants]{left, top, right, bottom, ten_bit ? 65535u : 255u, temporal, (right - left + 15) / 16, anchor_bytes_};
    // The previous dispatch's anchors and counters are written before this one reads them.
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; barrier.UAV.pResource = history_;
    list->ResourceBarrier(1, &barrier);
    list->SetComputeRootSignature(root_); list->SetPipelineState(pipeline_);
    list->SetComputeRoot32BitConstants(0, kConstants, constants, 0);
    list->SetComputeRootConstantBufferView(1, weights_->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, luma); list->SetComputeRootDescriptorTable(3, output);
    list->SetComputeRootUnorderedAccessView(4, history_->GetGPUVirtualAddress());
    list->Dispatch((right - left + 15) / 16, (bottom - top + 15) / 16, 1);
    if (fits) { // The counters, for the CPU once the list has run (TileStatistics).
        D3D12_RESOURCE_BARRIER t{}; t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition = {history_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &t);
        list->CopyBufferRegion(readback_, 0, history_, anchor_bytes_, kStatsBytes);
        std::swap(t.Transition.StateBefore, t.Transition.StateAfter); list->ResourceBarrier(1, &t);
    }
    ++records_; if (temporal & kReuse) ++reuse_records_;
    valid_ = fits;
    last_[0] = left; last_[1] = top; last_[2] = right; last_[3] = bottom; last_ten_bit_ = ten_bit;
}
NeuralUpscalePass::TileCounts NeuralUpscalePass::TileStatistics() const noexcept {
    if (!counters_) return {};
    return {counters_[0], counters_[1]};
}
void NeuralUpscalePass::Release() noexcept {
    if (readback_ && counters_) { const D3D12_RANGE none{0, 0}; readback_->Unmap(0, &none); }
    counters_ = nullptr; Drop(readback_);
    Drop(pipeline_); Drop(root_); Drop(weights_); Drop(history_); valid_ = false; history_width_ = history_height_ = 0;
}
}
