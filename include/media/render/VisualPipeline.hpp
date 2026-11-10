#pragma once
#include <d3d12.h>
#include <cstdint>

namespace media::render {
// Music mode's picture (docs/audio-dsp.md, "Music mode"): what VisualComposite.hlsl draws, as 56 root constants. Bands and
// the waveform envelope are bytes: band b in byte b of `bands`; column c's least and greatest sample (biased by 128) in
// bytes 2c and 2c + 1 of `wave`.
struct VisualUniforms {
    float width = 0, height = 0, time = 0, level = 0; // The target's size in pixels, seconds, the smoothed level (0..1).
    float peak = 0, scale = 1, dim = 0, reserved = 0;  // The smoothed peak, the DPI scale, 1 while paused.
    std::uint32_t bands[8]{};
    std::uint32_t wave[40]{};
};
static_assert(sizeof(VisualUniforms) == 56 * 4);
// A full-target triangle; opaque (it is the background the controls are drawn over). Created once per device.
class VisualPipeline final {
public:
    HRESULT Create(ID3D12Device*) noexcept;
    void Record(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height, const VisualUniforms&) const noexcept;
    void Release() noexcept;
    [[nodiscard]] bool Ready() const noexcept { return pipeline_ != nullptr; }
private:
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
};
}
