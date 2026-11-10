#pragma once
#include "media/ui/StatusCue.hpp"
#include "media/core/linear_memory_arena.hpp"
#include "media/core/spsc_ring_buffer.hpp"
#include <d3d12.h>
#include <atomic>
namespace media::render {
// The status pill's GPU pass (the HUD's companion, like PalettePipeline): glass, rim and icon are analytical geometry
// from the cue's uniforms; the text is laid out by DirectWrite on a text thread at its exact pixel size (font fallback
// and bidi for any script), as one byte of coverage per pixel, copied into a fence-guarded upload region and read by
// the pixel shader through a root buffer view. One raster in flight; a newer text waits for it. The text thread is
// the only user of fonts; nothing allocates from the process heap after Prepare.
class StatusPipeline final {
public:
    static constexpr unsigned kMaxWidth = 2048, kMaxHeight = 384, kMaxUnits = ui::StatusCue::kMaxBytes; // A panel of kMaxLines at 2x.
    static constexpr unsigned kRegions = 3, kRegionBytes = kMaxWidth * kMaxHeight;
    static constexpr float kMargin = 40; // Design px of scissor around the pill (its shadow decays inside).
    struct Timeline { std::uint64_t completed = 0, next = 0; };
    struct Statistics {
        unsigned requests = 0, rasters = 0, uploads = 0, failures = 0;
        unsigned width = 0, height = 0;   // The raster on screen.
        float ink_left = 0, ink_right = 0; // Its text's horizontal extent in the raster.
        std::uint32_t revision = 0;       // The cue revision on screen.
        HRESULT error = S_OK;
    };
    StatusPipeline() noexcept = default;
    StatusPipeline(const StatusPipeline&) = delete;
    StatusPipeline& operator=(const StatusPipeline&) = delete;
    ~StatusPipeline() noexcept { Release(); }
    HRESULT Prepare(ID3D12Device*, core::LinearMemoryArena&) noexcept; // Preparing only; starts the text thread.
    HRESULT Recreate(ID3D12Device*) noexcept;
    void ReleaseGpu() noexcept;
    void Release() noexcept;
    bool Ready() const noexcept { return staging_ != nullptr && pipeline_ != nullptr; }
    // Presentation thread, non-blocking: asks for the raster of the cue's text (when it or the lane changed), collects
    // a finished one and tells the cue it can appear (StatusCue::Ready).
    void Update(ui::StatusCue&, std::int64_t now) noexcept;
    bool Dirty() const noexcept { return pending_upload_; }
    void Record(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height,
                const ui::StatusCue&, Timeline = {}) noexcept;
    bool Settle(ui::StatusCue&, std::int64_t now, unsigned timeout_ms) noexcept; // Tests: the cue's raster is ready (Ready at `now`).
    const Statistics& Stats() const noexcept { return stats_; }
private:
    struct Key { std::uint32_t revision = 0, width = 0, height = 0; float font = 0; bool valid = false; };
    struct Request { std::uint32_t units, width, height; float font; };
    struct Completion { std::uint32_t width, height, stride; float ink_left, ink_right; std::int32_t result; };
    static DWORD WINAPI TextEntry(void*) noexcept;
    void TextThread() noexcept;
    HRESULT CreateGpu(ID3D12Device*) noexcept;
    void Collect(ui::StatusCue*, std::int64_t now) noexcept;
    static Key KeyOf(const ui::StatusCue&) noexcept;
    static bool Same(const Key& a, const Key& b) noexcept {
        return a.valid && b.valid && a.revision == b.revision && a.width == b.width && a.height == b.height && a.font == b.font;
    }
    wchar_t* request_text_ = nullptr;
    std::uint8_t* staging_ = nullptr;
    core::SpscRingBuffer<Request, 2>* requests_ = nullptr;
    core::SpscRingBuffer<Completion, 2>* completions_ = nullptr;
    HANDLE thread_ = nullptr, wake_ = nullptr;
    std::atomic<bool> stop_{false}, running_{false};
    std::atomic<HRESULT> thread_error_{S_OK};
    Key sent_{}, staged_key_{}, shown_key_{};
    Completion staged_{}, shown_{};
    bool in_flight_ = false, pending_upload_ = false;
    Statistics stats_{};
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
    ID3D12Resource* upload_ = nullptr;
    std::uint8_t* mapped_ = nullptr;
    std::uint64_t region_fence_[kRegions]{};
    unsigned current_ = 0;
};
}
