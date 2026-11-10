#include "media/render/ColorPipeline.hpp"
#include "Nv12Shader.hpp"
#include "HdrShader.hpp"
#include "Nv12CompareShader.hpp"
#include "HdrCompareShader.hpp"
#include "Nv12PictureShader.hpp"
#include "HdrPictureShader.hpp"
#include "Nv12ComparePictureShader.hpp"
#include "HdrComparePictureShader.hpp"
#include "HdrPeakShader.hpp"
#include "HdrDetectShader.hpp"
#include "SceneStatsShader.hpp"
#include "StatsResetShader.hpp"
#include "GridShader.hpp"
#include "VibroShader.hpp"
#include "TensorShader.hpp"
#include <algorithm>
#include <utility>

namespace media::render {
namespace {
template <class T> void ReleaseCom(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
}
HRESULT ColorPipeline::Create(ID3D12Device* device) noexcept {
    if (root_ || !device) return E_INVALIDARG;
    D3D12_DESCRIPTOR_RANGE ranges[3]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0},
                                     {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, 0}}; // t2: neural 2x luma.
    D3D12_ROOT_PARAMETER parameters[7]{};
    for (unsigned i = 0; i != 2; ++i) { parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[i].DescriptorTable = {1, &ranges[i]}; }
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[2].Constants = {0, 0, sizeof(ColorConstants) / 4};
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[3].Descriptor = {1, 0}; // u1: HDR peak state.
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[4].DescriptorTable = {1, &ranges[2]};
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[5].Descriptor = {2, 0}; // u2: micro-motion's reference.
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[6].Descriptor = {3, 0}; // u3: the statistics' blocks.
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_ROOT_SIGNATURE_DESC signature{7, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    ReleaseCom(blob); ReleaseCom(errors);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = root_;
    pipeline.CS = {g_Nv12Shader, sizeof(g_Nv12Shader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&sdr_));
    pipeline.CS = {g_HdrShader, sizeof(g_HdrShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&hdr_));
    pipeline.CS = {g_Nv12CompareShader, sizeof(g_Nv12CompareShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&sdr_compare_));
    pipeline.CS = {g_HdrCompareShader, sizeof(g_HdrCompareShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&hdr_compare_));
    // Picture controls and micro-motion (Reconstruct.hlsli PICTURE_CONTROLS), alone and with the split comparison.
    pipeline.CS = {g_Nv12PictureShader, sizeof(g_Nv12PictureShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&sdr_picture_));
    pipeline.CS = {g_HdrPictureShader, sizeof(g_HdrPictureShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&hdr_picture_));
    pipeline.CS = {g_Nv12ComparePictureShader, sizeof(g_Nv12ComparePictureShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&sdr_compare_picture_));
    pipeline.CS = {g_HdrComparePictureShader, sizeof(g_HdrComparePictureShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&hdr_compare_picture_));
    pipeline.CS = {g_HdrPeakShader, sizeof(g_HdrPeakShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&peak_pass_));
    pipeline.CS = {g_HdrDetectShader, sizeof(g_HdrDetectShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&detect_pass_));
    pipeline.CS = {g_SceneStatsShader, sizeof(g_SceneStatsShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&stats_pass_));
    pipeline.CS = {g_StatsResetShader, sizeof(g_StatsResetShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&stats_reset_));
    pipeline.CS = {g_GridShader, sizeof(g_GridShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&grid_pass_));
    pipeline.CS = {g_VibroShader, sizeof(g_VibroShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&vibro_pass_));
    pipeline.CS = {g_TensorShader, sizeof(g_TensorShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&tensor_pass_));
    // Committed default-heap buffers start zeroed: frame count 0 selects the initial 1000 cd/m2 peak.
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = 256;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    buffer.Width = 64 + kHorizonBytes; // The state, the statistics, the horizon's histograms (HdrToneMap.hlsl).
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON,
                                                            nullptr, IID_PPV_ARGS(&peak_));
    buffer.Width = 262144 * 4; // HdrToneMap.hlsl kMaxBlocks: one word per block.
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON,
                                                            nullptr, IID_PPV_ARGS(&blocks_));
    if (FAILED(hr)) Release();
    return hr;
}
void ColorPipeline::Bind(ID3D12GraphicsCommandList* list, D3D12_GPU_DESCRIPTOR_HANDLE planes, D3D12_GPU_DESCRIPTOR_HANDLE target,
                         const ColorConstants& constants, D3D12_GPU_DESCRIPTOR_HANDLE neural) const noexcept {
    list->SetComputeRootSignature(root_);
    list->SetComputeRootDescriptorTable(0, planes);
    list->SetComputeRootDescriptorTable(1, target);
    list->SetComputeRoot32BitConstants(2, sizeof(ColorConstants) / 4, &constants, 0);
    list->SetComputeRootUnorderedAccessView(3, peak_->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(4, neural.ptr ? neural : planes);
    list->SetComputeRootUnorderedAccessView(5, (motion_ ? motion_ : peak_)->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(6, blocks_->GetGPUVirtualAddress());
}
void ColorPipeline::Record(ID3D12GraphicsCommandList* list, D3D12_GPU_DESCRIPTOR_HANDLE planes, D3D12_GPU_DESCRIPTOR_HANDLE target,
                           const ColorConstants& constants, D3D12_GPU_DESCRIPTOR_HANDLE neural) const noexcept {
    const bool hdr = IsHdrTransfer(constants.transfer);
    Bind(list, planes, target, constants, neural);
    if (hdr && constants.peak_mode == 0) {
        // This picture's own peak (Detect), folded into the smoothed source peak (Peak) before the conversion reads it.
        // Barriers between them: each reads what the previous one wrote, also across conversions in one command list.
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; barrier.UAV.pResource = peak_;
        list->ResourceBarrier(1, &barrier);
        const unsigned sites_x = (constants.right + 1) / 2 - constants.left / 2, sites_y = (constants.bottom + 1) / 2 - constants.top / 2;
        list->SetPipelineState(detect_pass_); list->Dispatch((sites_x + 15) / 16, (sites_y + 15) / 16, 1);
        list->ResourceBarrier(1, &barrier);
        list->SetPipelineState(peak_pass_); list->Dispatch(1, 1, 1);
        list->ResourceBarrier(1, &barrier);
    }
    const bool compare = (constants.filter_flags & kCompare) != 0; // The split-comparison variant (Reconstruct.hlsli).
    const bool picture = constants.picture_flags != 0 || (constants.filter_flags & kRotate) != 0; // The picture-controls variants.
    ID3D12PipelineState* const variants[2][2][2]{{{sdr_, sdr_picture_}, {sdr_compare_, sdr_compare_picture_}},
                                                 {{hdr_, hdr_picture_}, {hdr_compare_, hdr_compare_picture_}}};
    list->SetPipelineState(variants[hdr][compare][picture]);
    list->Dispatch((constants.width + 15) / 16, (constants.height + 15) / 16, 1); // Reconstruct.hlsli: 16x16 tiles.
}
void ColorPipeline::RecordStats(ID3D12GraphicsCommandList* list, const ColorConstants& constants, ID3D12Resource* readback,
                                std::uint64_t offset, ID3D12Resource* horizon, std::uint64_t horizon_offset) const noexcept {
    // The conversion read the peak state (a UAV): wait for it, add this picture's totals, then copy for the CPU. The buffer
    // decays to the common state when the list ends, as before.
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; barrier.UAV.pResource = peak_;
    list->ResourceBarrier(1, &barrier);
    const unsigned width = constants.right - constants.left, height = constants.bottom - constants.top;
    list->SetPipelineState(stats_pass_); list->Dispatch((width + 63) / 64, (height + 63) / 64, 1);
    D3D12_RESOURCE_BARRIER copy{}; copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy.Transition = {peak_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &copy);
    list->CopyBufferRegion(readback, offset, peak_, 8, kStatsBytes);
    if (horizon && (constants.filter_flags & kHorizon)) list->CopyBufferRegion(horizon, horizon_offset, peak_, 64, kHorizonBytes); // HdrToneMap.hlsl kHorizonBase.
    std::swap(copy.Transition.StateBefore, copy.Transition.StateAfter);
    list->ResourceBarrier(1, &copy); // Later passes of this list (the test load) bind it as a UAV again.
    list->SetPipelineState(stats_reset_); list->Dispatch(1, 1, 1); // The per-picture fields, for the next picture.
    list->ResourceBarrier(1, &barrier);
}
void ColorPipeline::RecordGrid(ID3D12GraphicsCommandList* list, const ColorConstants& constants, unsigned scale, unsigned words, unsigned rows,
                               ID3D12Resource* grid, ID3D12Resource* readback, std::uint64_t offset) const noexcept {
    ColorConstants c = constants; c.width = scale; c.height = words; // The grid's own two parameters (HdrToneMap.hlsl Grid).
    list->SetComputeRoot32BitConstants(2, sizeof(ColorConstants) / 4, &c, 0);
    list->SetComputeRootUnorderedAccessView(6, grid->GetGPUVirtualAddress());
    list->SetPipelineState(grid_pass_); list->Dispatch((words + 7) / 8, (rows + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER copy{}; copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy.Transition = {grid, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &copy);
    list->CopyBufferRegion(readback, offset, grid, 0, std::uint64_t{words} * 4 * rows);
    std::swap(copy.Transition.StateBefore, copy.Transition.StateAfter);
    list->ResourceBarrier(1, &copy);
    list->SetComputeRoot32BitConstants(2, sizeof(ColorConstants) / 4, &constants, 0); // Later passes of this list see the conversion's.
    list->SetComputeRootUnorderedAccessView(6, blocks_->GetGPUVirtualAddress());
}
void ColorPipeline::RecordVibro(ID3D12GraphicsCommandList* list, const ColorConstants& constants, float left, float top, float right, float bottom,
                                unsigned cells, ID3D12Resource* buffer, ID3D12Resource* readback, std::uint64_t offset) const noexcept {
    ColorConstants c = constants; c.width = cells; // The region in luma samples of the coded surface (HdrToneMap.hlsl Vibro).
    c.view_m[0] = left; c.view_m[1] = top; c.view_m[2] = right; c.view_m[3] = bottom;
    list->SetComputeRoot32BitConstants(2, sizeof(ColorConstants) / 4, &c, 0);
    list->SetComputeRootUnorderedAccessView(6, buffer->GetGPUVirtualAddress());
    list->SetPipelineState(vibro_pass_); list->Dispatch(cells, cells, 1);
    D3D12_RESOURCE_BARRIER copy{}; copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &copy);
    list->CopyBufferRegion(readback, offset, buffer, 0, std::uint64_t{cells} * cells * kVibroValues * 4);
    std::swap(copy.Transition.StateBefore, copy.Transition.StateAfter);
    list->ResourceBarrier(1, &copy);
    list->SetComputeRoot32BitConstants(2, sizeof(ColorConstants) / 4, &constants, 0);
    list->SetComputeRootUnorderedAccessView(6, blocks_->GetGPUVirtualAddress());
}
void ColorPipeline::RecordTensor(ID3D12GraphicsCommandList* list, const ColorConstants& constants, float left, float top, float right, float bottom,
                                 unsigned width, unsigned height, ID3D12Resource* buffer, ID3D12Resource* readback, bool peak_in_use) const noexcept {
    ColorConstants c = constants; c.width = width; c.height = height; // The region in luma samples of the coded surface (HdrToneMap.hlsl Tensor).
    c.view_m[0] = left; c.view_m[1] = top; c.view_m[2] = right; c.view_m[3] = bottom;
    list->SetComputeRoot32BitConstants(2, sizeof(ColorConstants) / 4, &c, 0);
    list->SetComputeRootUnorderedAccessView(6, buffer->GetGPUVirtualAddress());
    list->SetPipelineState(tensor_pass_); list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER copy{}; copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &copy);
    list->CopyBufferRegion(readback, 0, buffer, 0, std::uint64_t{width} * height * 12);
    std::swap(copy.Transition.StateBefore, copy.Transition.StateAfter);
    list->ResourceBarrier(1, &copy);
    list->SetComputeRoot32BitConstants(2, sizeof(ColorConstants) / 4, &constants, 0);
    list->SetComputeRootUnorderedAccessView(6, blocks_->GetGPUVirtualAddress());
    if (!IsHdrTransfer(constants.transfer)) return;
    // The source peak the picture is converted with (PQ, a float), after the means: what the CPU's tone map takes.
    D3D12_RESOURCE_BARRIER source{}; source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    source.Transition = {peak_, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    if (peak_in_use) list->ResourceBarrier(1, &source);
    list->CopyBufferRegion(readback, std::uint64_t{width} * height * 12, peak_, 4, 4);
    if (peak_in_use) { std::swap(source.Transition.StateBefore, source.Transition.StateAfter); list->ResourceBarrier(1, &source); }
}
HRESULT ColorPipeline::PrepareMotion(ID3D12Device* device, std::uint64_t pixels) noexcept {
    if (!device || !root_) return E_INVALIDARG;
    if (motion_ && motion_pixels_ >= pixels) return S_OK;
    ReleaseCom(motion_); motion_pixels_ = 0;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = (std::max)(pixels, std::uint64_t{64}) * 4;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&motion_));
    if (SUCCEEDED(hr)) motion_pixels_ = pixels;
    return hr;
}
void ColorPipeline::Release() noexcept {
    ReleaseCom(sdr_picture_); ReleaseCom(hdr_picture_); ReleaseCom(sdr_compare_picture_); ReleaseCom(hdr_compare_picture_);
    ReleaseCom(stats_reset_); ReleaseCom(grid_pass_); ReleaseCom(vibro_pass_); ReleaseCom(tensor_pass_); ReleaseCom(blocks_); ReleaseCom(motion_); motion_pixels_ = 0; ReleaseCom(stats_pass_); ReleaseCom(peak_); ReleaseCom(peak_pass_); ReleaseCom(detect_pass_); ReleaseCom(hdr_compare_); ReleaseCom(sdr_compare_); ReleaseCom(hdr_); ReleaseCom(sdr_); ReleaseCom(root_);
}
}
