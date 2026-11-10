#include "media/render/HudPipeline.hpp"
#include "HudVertexShader.hpp"
#include "HudPixelShader.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
namespace media::render {
HRESULT HudPipeline::Create(ID3D12Device* device) noexcept {
    if (!device || root_) return E_INVALIDARG;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = parameters[1].ParameterType = parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, sizeof(ui::HudUniforms) / sizeof(float)};
    parameters[1].Constants = {1, 0, sizeof(ui::TimelineMarks::packed) / sizeof(std::uint32_t)};
    parameters[2].Constants = {2, 0, sizeof(ui::BookmarkMarks::packed) / sizeof(std::uint32_t)};
    parameters[0].ShaderVisibility = parameters[1].ShaderVisibility = parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    if (blob) blob->Release();
    if (errors) errors->Release();
    if (FAILED(hr)) return hr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root_;
    p.VS = {g_HudVertexShader, sizeof(g_HudVertexShader)}; p.PS = {g_HudPixelShader, sizeof(g_HudPixelShader)};
    auto& blend = p.BlendState.RenderTarget[0]; blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = UINT_MAX; p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1; p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; p.SampleDesc.Count = 1;
    return device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline_));
}
void HudPipeline::Record(ID3D12GraphicsCommandList* commands, D3D12_CPU_DESCRIPTOR_HANDLE target,
                         unsigned width, unsigned height, const ui::HudUniforms& hud, const ui::TimelineMarks& marks,
                         const ui::BookmarkMarks& bookmarks) const noexcept {
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const float extent = (hud.dock_half_width + kHorizontalMargin) * hud.scale;
    const float vertical = (hud.dock_half_height + kVerticalMargin) * hud.scale;
    const D3D12_RECT scissor{
        static_cast<LONG>((std::max)(0.0f, std::floor(hud.center_x - extent))),
        static_cast<LONG>((std::max)(0.0f, std::floor(hud.center_y - vertical))),
        static_cast<LONG>((std::min)(static_cast<float>(width), std::ceil(hud.center_x + extent))),
        static_cast<LONG>((std::min)(static_cast<float>(height), std::ceil(hud.center_y + vertical)))};
    commands->SetPipelineState(pipeline_); commands->SetGraphicsRootSignature(root_);
    commands->SetGraphicsRoot32BitConstants(0, sizeof(ui::HudUniforms) / sizeof(float), &hud, 0);
    commands->SetGraphicsRoot32BitConstants(1, sizeof(marks.packed) / sizeof(std::uint32_t), marks.packed, 0);
    commands->SetGraphicsRoot32BitConstants(2, sizeof(bookmarks.packed) / sizeof(std::uint32_t), bookmarks.packed, 0);
    commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); commands->DrawInstanced(3, 1, 0, 0);
}
void HudPipeline::Release() noexcept {
    if (pipeline_) { pipeline_->Release(); pipeline_ = nullptr; }
    if (root_) { root_->Release(); root_ = nullptr; }
}
}
