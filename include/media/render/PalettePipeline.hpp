#pragma once
#include "media/ui/CommandPalette.hpp"
#include "media/core/linear_memory_arena.hpp"
#include "media/core/spsc_ring_buffer.hpp"
#include <d3d12.h>
#include <atomic>
namespace media::render {
// The command bar's GPU pass (the HUD's companion: same premultiplied composite, its own pipeline state).
// - The bar, its icons, the match counter and the caret are analytical geometry from the palette's uniforms.
// - The typed text is laid out by DirectWrite (font fallback, bidi, joining: any script an input method can
//   produce) on a text thread, at the bar's exact pixel size, as one byte of coverage per pixel in a fixed arena
//   buffer. The presentation thread copies a finished raster into a fence-guarded region of an upload buffer, and
//   the pixel shader reads it there through a root buffer view: no texture, no descriptor, no copy on the GPU.
// - At most one raster is in flight; a newer text waits for it and then replaces it (keystrokes coalesce).
// The text thread is the only user of fonts. It and the presentation thread meet in two SPSC rings; neither waits
// for the other, and nothing here allocates from the process heap after Prepare.
class PalettePipeline final {
public:
    // Largest text lane, pixels: the bar at a layout scale of 3.9 (wide) and 4.5 (tall). Beyond that the raster
    // covers the lane's left part and its vertical middle.
    static constexpr unsigned kMaxWidth = 2048, kMaxHeight = 256;
    static constexpr unsigned kMaxUnits = 644;                     // UTF-16 units of text plus composition.
    static constexpr unsigned kRegions = 3, kRegionBytes = kMaxWidth * kMaxHeight;
    // Scissor margin around the bar, design pixels: its shadow has fully decayed inside it.
    static constexpr float kMargin = 84;
    // GPU progress of the queue the command lists run on. {0, 0}: the caller waits for each list before the next.
    struct Timeline { std::uint64_t completed = 0, next = 0; };
    struct Statistics {
        unsigned requests = 0, rasters = 0, uploads = 0, failures = 0;
        unsigned width = 0, height = 0; // The raster on screen.
        float caret = 0;                // Its caret, pixels from the lane's left edge.
        float ink_left = 0, ink_right = 0; // Horizontal extent of its text.
        bool right_to_left = false;
        HRESULT error = S_OK;           // The text thread's last failure.
    };
    PalettePipeline() noexcept = default;
    PalettePipeline(const PalettePipeline&) = delete;
    PalettePipeline& operator=(const PalettePipeline&) = delete;
    ~PalettePipeline() noexcept { Release(); }
    // Preparing only: reserves the raster and text buffers in the arena, creates the GPU objects and starts the text
    // thread (which creates its DirectWrite objects before this returns).
    HRESULT Prepare(ID3D12Device*, core::LinearMemoryArena&) noexcept;
    HRESULT Recreate(ID3D12Device*) noexcept; // Device-loss recovery; the text is rasterised again.
    void ReleaseGpu() noexcept;
    void Release() noexcept;
    bool Ready() const noexcept { return staging_ != nullptr && pipeline_ != nullptr; }
    // Presentation thread, cheap and non-blocking: asks for a raster when the shown text, the lane or the font size
    // changed, and collects a finished one.
    void Update(const ui::CommandPalette&) noexcept;
    // A finished raster waits to be drawn: the next Record shows it.
    bool Dirty() const noexcept { return pending_upload_; }
    void Record(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height,
                const ui::CommandPalette&, Timeline = {}) noexcept;
    // Setup, tests and tools only (this waits): the raster of the palette's current text is finished.
    bool Settle(const ui::CommandPalette&, unsigned timeout_ms) noexcept;
    const Statistics& Stats() const noexcept { return stats_; }
private:
    struct Key { std::uint32_t revision = 0, width = 0, height = 0; float font = 0; bool valid = false; };
    struct Request { std::uint32_t units, committed, width, height; float font; };
    struct Completion { std::uint32_t width, height, stride, right_to_left; float caret, ink_left, ink_right; std::int32_t result; };
    static DWORD WINAPI TextEntry(void*) noexcept;
    void TextThread() noexcept;
    HRESULT CreateGpu(ID3D12Device*) noexcept;
    void Collect() noexcept;
    static Key KeyOf(const ui::CommandPalette&) noexcept;
    static bool Same(const Key& a, const Key& b) noexcept {
        return a.valid && b.valid && a.revision == b.revision && a.width == b.width && a.height == b.height && a.font == b.font;
    }
    // Shared through the rings: the request's text (written before its request is published) and the raster
    // (written by the text thread before its completion is published).
    wchar_t* request_text_ = nullptr;
    std::uint8_t* staging_ = nullptr;
    core::SpscRingBuffer<Request, 2>* requests_ = nullptr;       // Presentation -> text.
    core::SpscRingBuffer<Completion, 2>* completions_ = nullptr; // Text -> presentation.
    HANDLE thread_ = nullptr, wake_ = nullptr;
    std::atomic<bool> stop_{false}, running_{false};
    std::atomic<HRESULT> thread_error_{S_OK};
    // Presentation thread.
    Key sent_{}, staged_key_{}, shown_key_{};
    Completion staged_{}, shown_{};
    bool in_flight_ = false, pending_upload_ = false;
    Statistics stats_{};
    // GPU.
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
    ID3D12Resource* upload_ = nullptr;
    std::uint8_t* mapped_ = nullptr;
    std::uint64_t region_fence_[kRegions]{};
    unsigned current_ = 0;
};
}
