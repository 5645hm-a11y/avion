#pragma once
#include "media/video/NeuralUpscale.hpp"
#include <d3d12.h>
#include <cstdint>
namespace media::render {
// GPU pass of the micro-ESPCN 2x luma upscaler (NeuralSuperRes.hlsl). Create uploads the weights once into a
// default-heap constant buffer and creates the temporal history (setup only: it waits for the copy); Record only binds
// and dispatches. The caller binds a shader-visible heap holding the luma-plane SRV and the output UAV (R16F or R32F, at
// least 2x the crop).
//
// Temporal tile reuse (docs/neural.md): each 16x16 tile keeps the 20x20 source codes it was last evaluated from (its
// anchor); a Record skips every tile whose neighbourhood matches its anchor, leaving its 32x32 output as it is. Exact
// matching is bit-exact; tolerant matching (one 8-bit step at most, a quarter of one on average) keeps the anchor's output
// under compression noise without letting differences add up. Either is right only if the output texture still holds
// what earlier Records wrote, so the contract is:
// - consecutive Records write the same output texture, executed in the order recorded (one queue);
// - the crop and the plane format are compared by the pass itself (a change evaluates every tile);
// - anything else that breaks the chain (another output texture, a new epoch, a recorded list that was not executed)
//   calls Invalidate first: the next Record then evaluates every tile.
class NeuralUpscalePass final {
public:
    NeuralUpscalePass() noexcept = default;
    NeuralUpscalePass(const NeuralUpscalePass&) = delete;
    NeuralUpscalePass& operator=(const NeuralUpscalePass&) = delete;
    ~NeuralUpscalePass() noexcept { Release(); }
    // `max_width` x `max_height`: the largest crop reuse covers (a larger one is evaluated whole, every time).
    [[nodiscard]] HRESULT Create(ID3D12Device*, ID3D12CommandQueue*, const video::NeuralModel&, unsigned max_width, unsigned max_height) noexcept;
    // Writes the 2x image of the crop rectangle [left, right) x [top, bottom) of the luma plane at the output's origin.
    // `ten_bit`: the luma view is R16 (P010) instead of R8 (NV12).
    void Record(ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE luma, D3D12_GPU_DESCRIPTOR_HANDLE output,
                unsigned left, unsigned top, unsigned right, unsigned bottom, bool ten_bit = false) noexcept;
    // The next Record evaluates every tile (and starts a new history).
    void Invalidate() noexcept { valid_ = false; }
    // Off: every Record evaluates every tile (the reference behaviour, for comparison), anchors still kept. Exact (the
    // default): bit-identical to Off. Tolerant: what the renderer uses for playback.
    enum class TemporalReuse : std::uint8_t { Off, Exact, Tolerant };
    void SetReuse(TemporalReuse mode) noexcept { reuse_ = mode; }
    [[nodiscard]] TemporalReuse Reuse() const noexcept { return reuse_; }
    // Tiles reused and evaluated since Create (cumulative, as copied by the last Record whose list has run).
    struct TileCounts { std::uint32_t reused = 0, evaluated = 0; };
    [[nodiscard]] TileCounts TileStatistics() const noexcept;
    [[nodiscard]] bool Ready() const noexcept { return pipeline_ != nullptr; }
    // Records that were allowed to reuse tiles (the previous one valid, same crop and format), of all Records.
    [[nodiscard]] unsigned ReuseRecords() const noexcept { return reuse_records_; }
    [[nodiscard]] unsigned Records() const noexcept { return records_; }
    void Release() noexcept;
private:
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
    ID3D12Resource* weights_ = nullptr;
    ID3D12Resource* history_ = nullptr;  // Every tile's anchor (800 bytes), then the tile counters.
    ID3D12Resource* readback_ = nullptr; // The counters, copied after each Record; mapped.
    const std::uint32_t* counters_ = nullptr;
    unsigned history_width_ = 0, history_height_ = 0, anchor_bytes_ = 0;
    unsigned last_[4]{}; bool last_ten_bit_ = false, valid_ = false;
    TemporalReuse reuse_ = TemporalReuse::Exact;
    unsigned records_ = 0, reuse_records_ = 0;
};
}
