#pragma once
#include "media/ui/KineticHud.hpp"
#include <d3d12.h>
namespace media::render {
// Prepared native graphics pass; all geometry comes from SV_VertexID, the 128-byte HUD block (b0), the 64-byte timeline
// marks (b1) and the 64-byte bookmarks (b2): 64 root constants, the most a root signature holds.
class HudPipeline final {
public:
    // Scissor margins around the dock, design pixels. The drop shadow must have fully decayed inside them, or the
    // scissor would cut it into a visible edge over bright video.
    static constexpr float kHorizontalMargin = 56, kVerticalMargin = 40;
    HRESULT Create(ID3D12Device*) noexcept;
    void Record(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE target,
                unsigned width, unsigned height, const ui::HudUniforms&, const ui::TimelineMarks& = {}, const ui::BookmarkMarks& = {}) const noexcept;
    void Release() noexcept;
private:
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
};
}
