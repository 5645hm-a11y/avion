#include "media/render/VisualPipeline.hpp"
#include "VisualVertexShader.hpp"
#include "VisualPixelShader.hpp"
#include <d3dcompiler.h>
namespace media::render {
HRESULT VisualPipeline::Create(ID3D12Device* device) noexcept {
    if (!device || root_) return E_INVALIDARG;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants = {0, 0, sizeof(VisualUniforms) / sizeof(std::uint32_t)};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    if (blob) blob->Release();
    if (errors) errors->Release();
    if (FAILED(hr)) return hr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root_;
    p.VS = {g_VisualVertexShader, sizeof(g_VisualVertexShader)}; p.PS = {g_VisualPixelShader, sizeof(g_VisualPixelShader)};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; // Opaque.
    p.SampleMask = UINT_MAX; p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1; p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; p.SampleDesc.Count = 1;
    hr = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline_));
    if (FAILED(hr)) Release();
    return hr;
}
void VisualPipeline::Record(ID3D12GraphicsCommandList* commands, D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height,
                            const VisualUniforms& uniforms) const noexcept {
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commands->SetPipelineState(pipeline_); commands->SetGraphicsRootSignature(root_);
    commands->SetGraphicsRoot32BitConstants(0, sizeof(VisualUniforms) / sizeof(std::uint32_t), &uniforms, 0);
    commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); commands->DrawInstanced(3, 1, 0, 0);
}
void VisualPipeline::Release() noexcept {
    if (pipeline_) { pipeline_->Release(); pipeline_ = nullptr; }
    if (root_) { root_->Release(); root_ = nullptr; }
}
}
