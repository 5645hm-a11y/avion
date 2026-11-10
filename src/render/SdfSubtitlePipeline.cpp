#include "media/render/SdfSubtitlePipeline.hpp"
#include "SubtitleVertexShader.hpp"
#include "SubtitlePixelShader.hpp"
#include "SubtitleAtlasShader.hpp"
#include <d2d1.h>
#include <dwrite.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>

namespace media::render {
namespace {
constexpr unsigned kCell = 96, kColumns = 32, kHash = 65536, kNone = ~0u;
constexpr unsigned kSlots = 64, kSlotEdges = 4096;       // Staging outlines: one glyph each, 64 KiB.
constexpr unsigned kPrefetchCues = 64;                   // Upcoming cues considered per update, at most.
// Upload ring: four regions (one per composite in flight, plus one), each with the instances drawn, the cells
// generated and their outline edges.
constexpr unsigned kRegions = 4, kRegionQuads = 8192, kRegionEdges = 65536;
constexpr std::size_t kRangesOffset = std::size_t{kRegionQuads} * 32, kEdgesOffset = kRangesOffset + 4096;
constexpr std::size_t kRegionBytes = kEdgesOffset + std::size_t{kRegionEdges} * 16;
static_assert(kRegionBytes % 256 == 0 && kSlots == 64);
constexpr float kEm = SdfSubtitlePipeline::kLayoutEm, kPadding = 8;
enum : std::uint8_t { kIdle, kRequested, kOutlined, kResident, kEmpty, kBroken };
template<class T> void Drop(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
struct Range { std::uint32_t first, count, winding, cell; };
}
struct SdfSubtitlePipeline::Edge { float x0, y0, x1, y1; };
// One distinct (font face, glyph) the track uses (shaping thread). `empty`: no ink (spaces), never needs a cell.
struct SdfSubtitlePipeline::Key { IDWriteFontFace* face; UINT16 glyph; bool empty; };
// A shaped glyph: left edge of its advance box, baseline, run scale (em / layout em), distinct-glyph key.
struct SdfSubtitlePipeline::Glyph { float pen, baseline, scale; std::uint32_t key; };
struct SdfSubtitlePipeline::CueLayout { std::atomic<std::uint32_t> ready{0}; std::uint32_t first = 0, count = 0; float height = 0; };
// Presentation-thread state of a distinct glyph: where it is in the atlas and when it was last needed.
struct SdfSubtitlePipeline::Residency {
    std::uint32_t used = 0, active = 0, cell = kNone; // `used`/`active`: stamps of the last update needing it (soon/now).
    float x = 0, y = 0, scale = 1;                     // Cell origin and fit scale (layout em units).
    std::uint8_t state = kIdle;
};
namespace {
class Outline final : public IDWriteGeometrySink {
public:
    float (*edges)[4] = nullptr; unsigned count = 0, capacity = 0, winding = 1;
    float flatness2 = 0.0064f; // Squared flatness bound: 0.08 atlas pixels, coarsened only for extreme outlines.
    bool failed = false; D2D1_POINT_2F current{}, start{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
        if (!p) return E_POINTER;
        *p = nullptr; if (id != __uuidof(IUnknown) && id != __uuidof(ID2D1SimplifiedGeometrySink)) return E_NOINTERFACE;
        *p = static_cast<IDWriteGeometrySink*>(this); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE mode) override { winding = mode == D2D1_FILL_MODE_WINDING ? 1u : 0u; }
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}
    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) override { start = current = p; }
    void Line(D2D1_POINT_2F p) noexcept {
        if (p.x != current.x || p.y != current.y) {
            if (count >= capacity) failed = true;
            else { auto& e = edges[count++]; e[0] = current.x; e[1] = current.y; e[2] = p.x; e[3] = p.y; }
        }
        current = p;
    }
    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, UINT32 n) override { for (UINT32 i = 0; i < n; ++i) Line(points[i]); }
    void Curve(D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_POINT_2F p2, D2D1_POINT_2F p3, unsigned depth) noexcept {
        // Adaptive de Casteljau: below the flatness bound, at most 10 subdivisions deep.
        const float dx = p3.x - p0.x, dy = p3.y - p0.y;
        const float a = std::abs((p1.x - p0.x) * dy - (p1.y - p0.y) * dx);
        const float b = std::abs((p2.x - p0.x) * dy - (p2.y - p0.y) * dx);
        if (depth == 10 || (a + b) * (a + b) <= flatness2 * (dx * dx + dy * dy)) { Line(p3); return; }
        const auto mid = [](D2D1_POINT_2F l, D2D1_POINT_2F r) { return D2D1_POINT_2F{(l.x + r.x) * 0.5f, (l.y + r.y) * 0.5f}; };
        const auto a0 = mid(p0,p1), a1 = mid(p1,p2), a2 = mid(p2,p3), b0 = mid(a0,a1), b1 = mid(a1,a2), c = mid(b0,b1);
        Curve(p0,a0,b0,c,depth + 1); Curve(c,b1,a2,p3,depth + 1);
    }
    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* curves, UINT32 n) override {
        for (UINT32 i = 0; i < n; ++i) Curve(current,curves[i].point1,curves[i].point2,curves[i].point3,0);
    }
    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) override { Line(start); }
    HRESULT STDMETHODCALLTYPE Close() override { return failed ? E_BOUNDS : S_OK; }
};
unsigned PlainText(std::string_view text, wchar_t (&out)[4096]) noexcept {
    unsigned n = 0;
    while (!text.empty()) {
        if (text.front() == '<') { const auto end = text.find('>'); if (end != text.npos) { text.remove_prefix(end+1); continue; } }
        std::uint32_t scalar = 0;
        struct Entity { std::string_view name; std::uint32_t scalar; };
        constexpr Entity entities[]{{"&amp;",'&'},{"&lt;",'<'},{"&gt;",'>'},{"&nbsp;",0xa0},{"&lrm;",0x200e},{"&rlm;",0x200f},{"&quot;",'"'},{"&apos;",'\''}};
        for (const auto& e : entities) if (text.starts_with(e.name)) { scalar = e.scalar; text.remove_prefix(e.name.size()); break; }
        if (!scalar && !subtitle::Utf8(text,scalar)) return 0;
        if (scalar == '\r') { if (text.empty() || text.front() != '\n') scalar = '\n'; else continue; }
        if (n + (scalar > 0xffff ? 2u : 1u) > 4096) return 0;
        if (scalar > 0xffff) { scalar -= 0x10000; out[n++] = static_cast<wchar_t>(0xd800 + (scalar >> 10)); out[n++] = static_cast<wchar_t>(0xdc00 + (scalar & 1023)); }
        else out[n++] = static_cast<wchar_t>(scalar);
    }
    return n;
}
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) noexcept {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = {r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; return b;
}
HRESULT Root(ID3D12Device* d, const D3D12_ROOT_SIGNATURE_DESC& desc, ID3D12RootSignature** root) noexcept {
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);
    if (SUCCEEDED(hr)) hr = d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(root));
    Drop(blob); Drop(errors); return hr;
}
// A DirectWrite renderer (shaping thread): records every shaped glyph (fallback fonts, bidi, joining) and gives each
// distinct (font face, glyph) a key. Outlines are made later, only for the glyphs that are about to be shown.
class Collector final : public IDWriteTextRenderer {
public:
    using Key = SdfSubtitlePipeline::Key; using Glyph = SdfSubtitlePipeline::Glyph;
    Key* keys = nullptr; std::uint32_t* table = nullptr; Glyph* glyphs = nullptr;
    unsigned key_count = 0, glyph_count = 0, runs = 0, rtl_runs = 0, missing = 0, dropped = 0;
    IDWriteFontFace* faces[64]{}; unsigned face_count = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
        if (!p) return E_POINTER;
        *p = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IDWriteTextRenderer) && id != __uuidof(IDWritePixelSnapping)) return E_NOINTERFACE;
        *p = static_cast<IDWriteTextRenderer*>(this); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override { *m = {1,0,0,1,0,0}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override { *pixels = 1; return S_OK; }
    // Open-addressed lookup; beyond kMaxGlyphs distinct glyphs, further ones get no key (not drawn, counted).
    std::uint32_t Find(IDWriteFontFace* face, UINT16 glyph) noexcept {
        auto h = static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(face) >> 4) * 2654435761u ^ glyph * 2246822519u);
        for (unsigned probe = 0; probe < kHash; ++probe, ++h) {
            auto& slot = table[h & (kHash - 1)];
            if (!slot) {
                if (key_count == SdfSubtitlePipeline::kMaxGlyphs) return kNone;
                // Ink test from the font's metrics (no outline yet): a glyph whose black box has no area has no ink.
                DWRITE_GLYPH_METRICS m{};
                const bool empty = SUCCEEDED(face->GetDesignGlyphMetrics(&glyph, 1, &m, FALSE)) &&
                    (static_cast<INT32>(m.advanceWidth) - m.leftSideBearing - m.rightSideBearing <= 0 ||
                     static_cast<INT32>(m.advanceHeight) - m.topSideBearing - m.bottomSideBearing <= 0);
                face->AddRef(); keys[key_count] = {face, glyph, empty}; if (!glyph) ++missing;
                unsigned f = 0; while (f < face_count && faces[f] != face) ++f;
                if (f == face_count && face_count < 64) faces[face_count++] = face;
                slot = ++key_count; return key_count - 1;
            }
            const auto& key = keys[slot - 1];
            if (key.face == face && key.glyph == glyph) return slot - 1;
        }
        return kNone;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        if (run->isSideways) return E_NOTIMPL;
        ++runs; const bool rtl = (run->bidiLevel & 1) != 0; if (rtl) ++rtl_runs;
        const float scale = run->fontEmSize / kEm, direction = rtl ? -1.0f : 1.0f;
        for (UINT32 i = 0; i < run->glyphCount; ++i) {
            const auto offset = run->glyphOffsets ? run->glyphOffsets[i] : DWRITE_GLYPH_OFFSET{};
            const float advance = run->glyphAdvances[i];
            // Native glyph origins: an RTL pen points to the right-hand edge of its advance box.
            const float pen = x + direction * offset.advanceOffset - (rtl ? advance : 0);
            const auto key = Find(run->fontFace, run->glyphIndices[i]);
            if (key == kNone || glyph_count == SdfSubtitlePipeline::kMaxInstances) ++dropped;
            else glyphs[glyph_count++] = {pen, y - offset.ascenderOffset, scale, key};
            x += direction * advance;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return E_NOTIMPL; }
};
}
float SdfSubtitlePipeline::FontPixels(unsigned width, unsigned height, float dpi) noexcept {
    dpi = std::isfinite(dpi) ? std::clamp(dpi, 0.5f, 8.0f) : 1.0f;
    const float size = (std::max)(static_cast<float>(height) / 24, 18 * dpi);
    return (std::max)(1.0f, (std::min)(size, 0.92f * static_cast<float>(width) / kWrapEm));
}
HRESULT SdfSubtitlePipeline::Prepare(ID3D12Device* device, ID3D12CommandQueue* queue, core::LinearMemoryArena& arena,
                                     const subtitle::SubtitleTrack& track, unsigned min_cells) noexcept {
    if (!device || !queue || track_ || arena.IsSealed() || (track.Cues().empty() && !min_cells)) return E_INVALIDARG;
    // Cue layouts for any track that may be bound later (API 17): SubtitleTrack::kMaxCues of them when rebinding is possible.
    const auto count = min_cells ? static_cast<unsigned>(subtitle::SubtitleTrack::kMaxCues) : static_cast<unsigned>(track.Cues().size());
    // Atlas capacity: what the text can need (glyphs per UTF-8 byte stay below 2 even for complex scripts), at most
    // kAtlasCells; a larger track pages through it.
    std::uint64_t bytes = 0; for (const auto& cue : track.Cues()) bytes += cue.text.size();
    cells_ = static_cast<unsigned>((std::min)(std::uint64_t{kAtlasCells}, (2 * bytes + 64 + kColumns - 1) / kColumns * kColumns));
    cells_ = (std::min)(kAtlasCells, (std::max)(cells_, (min_cells + kColumns - 1) / kColumns * kColumns));
    keys_ = static_cast<Key*>(arena.Allocate(sizeof(Key) * kMaxGlyphs, alignof(Key)));
    table_ = static_cast<std::uint32_t*>(arena.Allocate(sizeof(std::uint32_t) * kHash));
    glyphs_ = static_cast<Glyph*>(arena.Allocate(sizeof(Glyph) * kMaxInstances, alignof(Glyph)));
    cues_ = static_cast<CueLayout*>(arena.Allocate(sizeof(CueLayout) * count, alignof(CueLayout)));
    slots_ = static_cast<Edge*>(arena.Allocate(sizeof(Edge) * kSlots * kSlotEdges, 16));
    residency_ = static_cast<Residency*>(arena.Allocate(sizeof(Residency) * kMaxGlyphs, alignof(Residency)));
    cell_key_ = static_cast<std::uint32_t*>(arena.Allocate(sizeof(std::uint32_t) * cells_));
    free_cells_ = static_cast<std::uint32_t*>(arena.Allocate(sizeof(std::uint32_t) * cells_));
    requests_ = arena.Create<core::SpscRingBuffer<Request, 64>>();
    completions_ = arena.Create<core::SpscRingBuffer<Completion, 64>>();
    if (!keys_ || !table_ || !glyphs_ || !cues_ || !slots_ || !residency_ || !cell_key_ || !free_cells_ || !requests_ || !completions_) {
        keys_ = nullptr; return E_OUTOFMEMORY;
    }
    std::memset(table_, 0, sizeof(std::uint32_t) * kHash);
    for (unsigned i = 0; i < count; ++i) std::construct_at(cues_ + i);
    cue_capacity_ = count; bound_.store(&track, std::memory_order_release); next_.store(nullptr); rebinding_ = nullptr;
    for (unsigned i = 0; i < kMaxGlyphs; ++i) std::construct_at(residency_ + i);
    track_ = &track;
    Reset();
    // The shaping thread starts now and creates the GPU objects first (D3D12 devices are free-threaded); Prepare
    // returns as soon as it runs. No font and no atlas memory is touched here.
    device_ = device; gpu_state_.store(0);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = wake_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr)) { thread_ = CreateThread(nullptr, 0, ShaperEntry, this, 0, nullptr); if (!thread_) hr = HRESULT_FROM_WIN32(GetLastError()); }
    if (FAILED(hr)) { Release(); return hr; }
    while (!running_.load()) Sleep(0); // Its CRT per-thread setup is done before measurement can start.
    return S_OK;
}
void SdfSubtitlePipeline::Reset() noexcept {
    for (unsigned c = 0; c < cells_; ++c) { cell_key_[c] = kNone; free_cells_[c] = cells_ - 1 - c; }
    free_count_ = cells_; resident_ = 0; cleared_ = false;
}
void SdfSubtitlePipeline::WaitGpu() const noexcept {
    while (thread_ && gpu_state_.load(std::memory_order_acquire) == 0) Sleep(1); // Setup-time creation, tens of ms.
}
HRESULT SdfSubtitlePipeline::Recreate(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    if (!device || !queue || !track_) return E_UNEXPECTED;
    ReleaseGpu();
    // Resident glyphs went with the old atlas. Glyphs being outlined or awaiting generation keep their cells.
    for (unsigned c = 0; c < cells_; ++c) {
        const auto key = cell_key_[c];
        if (key != kNone && residency_[key].state == kResident) { residency_[key].state = kIdle; cell_key_[c] = kNone; }
    }
    free_count_ = 0; resident_ = 0; cleared_ = false;
    for (unsigned c = cells_; c-- > 0;) if (cell_key_[c] == kNone) free_cells_[free_count_++] = c;
    for (auto& fence : region_fence_) fence = 0;
    const HRESULT hr = CreateGpu(device);
    gpu_error_.store(hr); gpu_state_.store(SUCCEEDED(hr) ? 1 : 2, std::memory_order_release);
    return hr;
}
HRESULT SdfSubtitlePipeline::CreateGpu(ID3D12Device* device) noexcept {
    atlas_width_ = kColumns * kCell; atlas_height_ = cells_ / kColumns * kCell;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{}; heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptors_));
    heap_desc.NumDescriptors = 1; heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&cpu_descriptors_));
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = atlas_width_; texture.Height = atlas_height_;
    texture.DepthOrArraySize = texture.MipLevels = 1; texture.SampleDesc.Count = 1; texture.Format = DXGI_FORMAT_R32_FLOAT;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&atlas_));
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = kRegions * kRegionBytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_));
    void* mapped = nullptr; const D3D12_RANGE none{0, 0};
    if (SUCCEEDED(hr)) hr = upload_->Map(0, &none, &mapped);
    mapped_ = static_cast<std::byte*>(mapped);
    if (SUCCEEDED(hr)) {
        descriptor_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format = texture.Format; view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        auto cpu = descriptors_->GetCPUDescriptorHandleForHeapStart();
        device->CreateUnorderedAccessView(atlas_, nullptr, &view, cpu);
        device->CreateUnorderedAccessView(atlas_, nullptr, &view, cpu_descriptors_->GetCPUDescriptorHandleForHeapStart());
        cpu.ptr += descriptor_stride_;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = texture.Format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(atlas_, &srv, cpu);
    }
    // Atlas generation: edges and cell ranges as root SRVs in the upload ring, the atlas as a UAV table.
    D3D12_DESCRIPTOR_RANGE uav{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
    D3D12_ROOT_PARAMETER cp[4]{};
    cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; cp[0].Constants = {0,0,4};
    cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; cp[1].Descriptor = {0,0};
    cp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; cp[2].Descriptor = {1,0};
    cp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; cp[3].DescriptorTable = {1,&uav};
    if (SUCCEEDED(hr)) hr = Root(device,{4,cp,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE},&compute_root_);
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{}; compute.pRootSignature = compute_root_; compute.CS = {g_SubtitleAtlasShader,sizeof(g_SubtitleAtlasShader)};
    if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&compute_pipeline_));
    D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,1};
    D3D12_ROOT_PARAMETER gp[3]{}; gp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; gp[0].Constants = {0,0,12};
    gp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; gp[1].Descriptor = {1,0}; gp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    gp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; gp[2].DescriptorTable = {1,&srv}; gp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    if (SUCCEEDED(hr)) hr = Root(device,{3,gp,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE},&graphics_root_);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{}; graphics.pRootSignature = graphics_root_;
    graphics.VS = {g_SubtitleVertexShader,sizeof(g_SubtitleVertexShader)}; graphics.PS = {g_SubtitlePixelShader,sizeof(g_SubtitlePixelShader)};
    auto& blend = graphics.BlendState.RenderTarget[0]; blend.BlendEnable = TRUE; blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics.SampleMask = UINT_MAX; graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; graphics.RasterizerState.DepthClipEnable = TRUE;
    graphics.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.NumRenderTargets = 1; graphics.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; graphics.SampleDesc.Count = 1;
    if (SUCCEEDED(hr)) hr = device->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(&graphics_pipeline_));
    if (FAILED(hr)) ReleaseGpu();
    return hr;
}
DWORD WINAPI SdfSubtitlePipeline::ShaperEntry(void* self) noexcept { static_cast<SdfSubtitlePipeline*>(self)->Shaper(); return 0; }
void SdfSubtitlePipeline::Shaper() noexcept {
    errno = 0; // Creates this thread's CRT data now, before Prepare returns (and before any allocation measurement).
    running_.store(true);
    {
        const HRESULT gpu = CreateGpu(device_);
        gpu_error_.store(gpu); device_ = nullptr; gpu_state_.store(SUCCEEDED(gpu) ? 1 : 2, std::memory_order_release);
    }
    IDWriteFactory* factory = nullptr; IDWriteTextFormat* format = nullptr;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory));
    constexpr float layout_width = kWrapEm * kEm;
    if (SUCCEEDED(hr)) hr = factory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,kEm,L"",&format);
    if (SUCCEEDED(hr)) hr = format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (SUCCEEDED(hr)) hr = format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (FAILED(hr)) shaper_error_.store(hr);
    Collector build;
    build.keys = keys_; build.table = table_; build.glyphs = glyphs_;
    auto cues = track_->Cues(); auto count = static_cast<unsigned>((std::min)(cues.size(), std::size_t{cue_capacity_}));
    unsigned cursor = 0, done = 0, rtl_lines = 0;
    const auto shape = [&](unsigned index) {
        auto& layout_cue = cues_[index];
        wchar_t text[4096]; WORD bidi[4096];
        const auto length = SUCCEEDED(hr) ? PlainText(cues[index].text, text) : 0; // 0: markup only, drawn as nothing.
        const unsigned first = build.glyph_count; float top = 0;
        if (length && GetStringTypeW(CT_CTYPE2, text, static_cast<int>(length), bidi)) {
            const auto strong = [&](unsigned begin, unsigned end, DWRITE_READING_DIRECTION fallback) {
                for (unsigned c = begin; c < end; ++c) {
                    if (bidi[c] == C2_RIGHTTOLEFT) return DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
                    if (bidi[c] == C2_LEFTTORIGHT) return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
                }
                return fallback;
            };
            const auto cue_direction = strong(0, length, DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
            // One layout per line: a layout has a single reading direction, a cue line is a bidi paragraph.
            for (unsigned begin = 0; begin < length;) {
                unsigned end = begin; while (end < length && text[end] != L'\n') ++end;
                if (end > begin) {
                    IDWriteTextLayout* layout = nullptr; DWRITE_TEXT_METRICS metrics{};
                    const auto direction = strong(begin, end, cue_direction);
                    HRESULT line = factory->CreateTextLayout(text + begin, end - begin, format, layout_width, 1e6f, &layout);
                    if (SUCCEEDED(line)) line = layout->SetReadingDirection(direction);
                    if (SUCCEEDED(line)) line = layout->GetMetrics(&metrics);
                    if (SUCCEEDED(line)) line = layout->Draw(nullptr, &build, 0, top);
                    if (SUCCEEDED(line) && direction == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT) ++rtl_lines;
                    if (FAILED(line)) ++build.dropped; // e.g. a sideways run: the line is left out.
                    top += metrics.height; Drop(layout);
                }
                begin = end + 1;
            }
        }
        layout_cue.first = first; layout_cue.count = build.glyph_count - first; layout_cue.height = top;
        layout_cue.ready.store(1, std::memory_order_release);
        shaping_.instances.store(build.glyph_count, std::memory_order_relaxed); shaping_.runs.store(build.runs, std::memory_order_relaxed);
        shaping_.rtl_runs.store(build.rtl_runs, std::memory_order_relaxed); shaping_.rtl_lines.store(rtl_lines, std::memory_order_relaxed);
        shaping_.missing.store(build.missing, std::memory_order_relaxed); shaping_.faces.store(build.face_count, std::memory_order_relaxed);
        shaping_.dropped.store(build.dropped, std::memory_order_relaxed);
        shaping_.unique.store(build.key_count, std::memory_order_release); // Last: a reader seeing the keys sees their data.
        shaped_.store(++done, std::memory_order_release);
    };
    const auto outline = [&](const Request& request) {
        // The glyph's outline, fitted into one atlas cell with an 8 px pad, into its staging slot. An extreme outline
        // is flattened more coarsely instead of failing.
        const auto& key = keys_[request.key];
        auto* edges = reinterpret_cast<float(*)[4]>(slots_ + std::size_t{request.slot} * kSlotEdges);
        Outline sink; sink.edges = edges; sink.capacity = kSlotEdges;
        HRESULT result = E_FAIL; const float advance = 0;
        for (unsigned attempt = 0; attempt < 5; ++attempt) {
            sink.count = 0; sink.failed = false; sink.flatness2 = 0.0064f * static_cast<float>(1u << (4 * attempt));
            result = key.face->GetGlyphRunOutline(kEm, &key.glyph, &advance, nullptr, 1, FALSE, FALSE, &sink);
            if (SUCCEEDED(result) && !sink.failed) break;
        }
        Completion done{request.key, request.slot, 0, sink.winding, 0, 0, 1, FAILED(result) || sink.failed ? 1u : 0u};
        if (!done.failed && sink.count) {
            float x0 = edges[0][0], x1 = x0, y0 = edges[0][1], y1 = y0;
            for (unsigned i = 0; i < sink.count; ++i) {
                const auto& e = edges[i]; x0 = (std::min)({x0,e[0],e[2]}); x1 = (std::max)({x1,e[0],e[2]});
                y0 = (std::min)({y0,e[1],e[3]}); y1 = (std::max)({y1,e[1],e[3]});
            }
            done.scale = (std::min)(1.0f, (kCell - 2 * kPadding) / (std::max)({x1-x0,y1-y0,1.0f}));
            done.x = x0 - kPadding / done.scale; done.y = y0 - kPadding / done.scale; done.count = sink.count;
            for (unsigned i = 0; i < sink.count; ++i) {
                auto& e = edges[i];
                e[0] = (e[0]-done.x)*done.scale; e[1] = (e[1]-done.y)*done.scale; e[2] = (e[2]-done.x)*done.scale; e[3] = (e[3]-done.y)*done.scale;
            }
        }
        return done;
    };
    while (!stop_.load(std::memory_order_acquire)) {
        bool worked = false;
        // A rebind (API 17): the presentation thread draws nothing of the old track any more. Its cue layouts and shaped glyphs
        // are forgotten; the glyph keys, their outlines and the atlas stay.
        if (const auto* next = next_.exchange(nullptr, std::memory_order_acq_rel)) {
            for (unsigned i = 0; i < cue_capacity_; ++i) cues_[i].ready.store(0, std::memory_order_relaxed);
            cues = next->Cues(); count = static_cast<unsigned>((std::min)(cues.size(), std::size_t{cue_capacity_}));
            build.glyph_count = 0; done = 0; cursor = 0;
            wanted_.store(0, std::memory_order_relaxed); shaped_.store(0, std::memory_order_release);
            bound_.store(next, std::memory_order_release);
            worked = true;
        }
        // Outline requests first: they are for cues about to be shown. One completion per request (both rings hold
        // 64 and at most 64 staging slots are outstanding), so a completion slot is always free.
        while (const auto* request = requests_->PeekReadSlot()) {
            auto* slot = completions_->AcquireWriteSlot();
            if (!slot) break;
            *slot = outline(*request);
            (void)completions_->CommitWriteSlot(); (void)requests_->ReleaseReadSlot(); worked = true;
        }
        if (done < count) {
            // Shape where playback is first (the presentation thread's wanted cue), then onwards, wrapping around.
            const auto want = wanted_.load(std::memory_order_relaxed);
            if (want < count && !cues_[want].ready.load(std::memory_order_relaxed)) cursor = want;
            while (cues_[cursor].ready.load(std::memory_order_relaxed)) cursor = (cursor + 1) % count;
            shape(cursor); worked = true;
        }
        if (!worked) (void)WaitForSingleObject(wake_, 50);
    }
    Drop(format); Drop(factory);
}
void SdfSubtitlePipeline::Collect() noexcept {
    while (const auto* completion = completions_->PeekReadSlot()) {
        const auto done = *completion; (void)completions_->ReleaseReadSlot();
        auto& r = residency_[done.key];
        if (done.failed || !done.count) {
            // No outline (or none DirectWrite can give): the glyph takes no cell.
            r.state = done.failed ? kBroken : kEmpty; broken_ += done.failed;
            cell_key_[r.cell] = kNone; free_cells_[free_count_++] = r.cell; r.cell = kNone;
            free_slots_ |= std::uint64_t{1} << done.slot;
        } else {
            r.state = kOutlined; r.x = done.x; r.y = done.y; r.scale = done.scale;
            pending_[pending_count_++] = {done.key, done.slot, done.count, done.winding};
        }
    }
}
bool SdfSubtitlePipeline::Admit(std::uint32_t key) noexcept {
    if (!free_slots_) return false;
    std::uint32_t cell = kNone;
    if (free_count_) cell = free_cells_[--free_count_];
    else {
        // Every cell is taken: reuse the least recently needed resident glyph that this update does not need.
        std::uint32_t oldest = stamp_;
        for (unsigned c = 0; c < cells_; ++c) {
            const auto k = cell_key_[c];
            if (k != kNone && residency_[k].state == kResident && residency_[k].used < oldest) { oldest = residency_[k].used; cell = c; }
        }
        if (cell == kNone) return false;
        residency_[cell_key_[cell]].state = kIdle; residency_[cell_key_[cell]].cell = kNone; --resident_; ++evictions_;
    }
    auto* request = requests_->AcquireWriteSlot();
    if (!request) { cell_key_[cell] = kNone; free_cells_[free_count_++] = cell; return false; }
    const auto slot = static_cast<std::uint32_t>(std::countr_zero(free_slots_));
    free_slots_ &= ~(std::uint64_t{1} << slot);
    *request = {key, slot}; (void)requests_->CommitWriteSlot();
    auto& r = residency_[key]; r.state = kRequested; r.cell = cell; cell_key_[cell] = key;
    return true;
}
bool SdfSubtitlePipeline::Rebind(const subtitle::SubtitleTrack* track) noexcept {
    if (!track || rebinding_ || !residency_ || !thread_) return false;
    track_ = nullptr; rebinding_ = track; wanted_sent_ = ~0u; // Nothing of the old track is drawn from here on.
    next_.store(track, std::memory_order_release);
    (void)SetEvent(wake_);
    return true;
}
void SdfSubtitlePipeline::Update(std::int64_t time) noexcept {
    if (rebinding_ && bound_.load(std::memory_order_acquire) == rebinding_) { track_ = rebinding_; rebinding_ = nullptr; } // Taken by the shaper.
    if (!track_ || !residency_) return;
    Collect();
    const auto cues = track_->Cues(); const auto count = static_cast<unsigned>((std::min)(cues.size(), std::size_t{cue_capacity_}));
    if (!count) return; // An empty track (a window ready for subtitles after open).
    ++stamp_;
    unsigned active[subtitle::SubtitleTrack::kMaxActive]{};
    const unsigned n = track_->Select(time, active);
    unsigned upcoming = 0, high = count; // First cue starting after `time`.
    while (upcoming < high) { const auto middle = upcoming + (high - upcoming) / 2; if (cues[middle].start <= time) upcoming = middle + 1; else high = middle; }
    // Where playback is: the shaping thread shapes from there.
    const auto want = n ? active[0] : (std::min)(upcoming, count - 1);
    bool wake = false;
    if (want != wanted_sent_) { wanted_.store(want, std::memory_order_relaxed); wanted_sent_ = want; wake = !cues_[want].ready.load(std::memory_order_acquire); }
    unsigned last = upcoming;
    while (last < count && last - upcoming < kPrefetchCues && cues[last].start <= time + kPrefetch) ++last;
    // Mark every glyph needed now or soon first, so that the requests below never evict one of them; then request the
    // missing ones, those shown now first and then in start order.
    const auto each = [&](unsigned cue, auto&& visit) {
        const auto& layout = cues_[cue];
        if (!layout.ready.load(std::memory_order_acquire)) return true;
        for (unsigned g = 0; g < layout.count; ++g) if (!visit(glyphs_[layout.first + g].key)) return false;
        return true;
    };
    for (unsigned i = 0; i < n; ++i) each(active[i], [&](std::uint32_t k) { residency_[k].used = residency_[k].active = stamp_; return true; });
    // Upcoming cues of the channels shown only: another language's glyphs would take cells for nothing.
    for (unsigned c = upcoming; c < last; ++c) if (track_->Shown(c)) each(c, [&](std::uint32_t k) { residency_[k].used = stamp_; return true; });
    const auto request = [&](std::uint32_t k) {
        auto& r = residency_[k];
        if (r.state != kIdle) return true;
        if (keys_[k].empty) { r.state = kEmpty; return true; }
        if (!Admit(k)) return false; // Out of staging slots or cells: the next update continues.
        wake = true; return true;
    };
    bool room = true;
    for (unsigned i = 0; room && i < n; ++i) room = each(active[i], request);
    for (unsigned c = upcoming; room && c < last; ++c) if (track_->Shown(c)) room = each(c, request);
    if (wake) (void)SetEvent(wake_);
}
bool SdfSubtitlePipeline::Resident(unsigned cue, bool outlined) const noexcept {
    const auto& layout = cues_[cue];
    if (!layout.ready.load(std::memory_order_acquire)) return false;
    for (unsigned g = 0; g < layout.count; ++g) {
        const auto state = residency_[glyphs_[layout.first + g].key].state;
        if (state != kResident && state != kEmpty && state != kBroken && !(outlined && state == kOutlined)) return false;
    }
    return true;
}
unsigned SdfSubtitlePipeline::Drawable(std::int64_t time, std::span<unsigned> out) const noexcept {
    if (!Ready()) return 0;
    unsigned active[subtitle::SubtitleTrack::kMaxActive]{};
    const unsigned n = track_->Select(time, active);
    unsigned drawable = 0;
    for (unsigned i = 0; i < n; ++i) if (Resident(active[i], true)) { if (drawable < out.size()) out[drawable] = active[i]; ++drawable; }
    return drawable;
}
unsigned SdfSubtitlePipeline::Record(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height,
                                      std::int64_t time, float bottom_inset, float dpi, Timeline timeline, std::span<unsigned> drawn) noexcept {
    if (rebinding_) Update(time); // A rebind is adopted here too (Record is the presentation thread's call).
    if (!Ready() || !width || !height) return 0;
    Update(time);
    // An upload region whose previous composite has retired on the GPU.
    unsigned region = kRegions;
    for (unsigned i = 0; i < kRegions && region == kRegions; ++i) {
        const auto candidate = (next_region_ + i) % kRegions;
        if (!timeline.next || region_fence_[candidate] <= timeline.completed) region = candidate;
    }
    if (region == kRegions) return 0;
    next_region_ = (region + 1) % kRegions; region_fence_[region] = timeline.next;
    std::byte* base = mapped_ + region * kRegionBytes;
    const auto gpu = upload_->GetGPUVirtualAddress() + region * kRegionBytes;
    ID3D12DescriptorHeap* heaps[]{descriptors_}; list->SetDescriptorHeaps(1, heaps);
    const auto table = descriptors_->GetGPUDescriptorHandleForHeapStart();
    if (!cleared_) {
        // Once per atlas: far outside everywhere, so filtering at a cell border never reads undefined texels.
        auto b = Transition(atlas_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); list->ResourceBarrier(1, &b);
        constexpr float outside[4]{64, 64, 64, 64};
        list->ClearUnorderedAccessViewFloat(table, cpu_descriptors_->GetCPUDescriptorHandleForHeapStart(), atlas_, outside, 0, nullptr);
        b = Transition(atlas_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); list->ResourceBarrier(1, &b);
        cleared_ = true;
    }
    // Generate outlined glyphs into their cells: those of the cues shown now first, within the per-composite budget.
    auto* ranges = reinterpret_cast<Range*>(base + kRangesOffset);
    auto* edges = reinterpret_cast<Edge*>(base + kEdgesOffset);
    unsigned generate = 0, edge_count = 0;
    for (unsigned pass = 0; pass != 2; ++pass)
        for (unsigned i = 0; i < pending_count_ && generate < kGeneratePerRecord;) {
            const auto p = pending_[i]; auto& r = residency_[p.key];
            if ((pass == 0) != (r.active == stamp_) || edge_count + p.count > kRegionEdges) { ++i; continue; }
            std::memcpy(edges + edge_count, slots_ + std::size_t{p.slot} * kSlotEdges, sizeof(Edge) * p.count);
            ranges[generate++] = {edge_count, p.count, p.winding, r.cell}; edge_count += p.count;
            r.state = kResident; ++resident_; ++generated_;
            free_slots_ |= std::uint64_t{1} << p.slot;
            pending_[i] = pending_[--pending_count_];
        }
    if (generate) {
        auto b = Transition(atlas_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); list->ResourceBarrier(1, &b);
        list->SetComputeRootSignature(compute_root_); list->SetPipelineState(compute_pipeline_);
        const unsigned constants[]{kColumns, kCell, 0, 0}; list->SetComputeRoot32BitConstants(0, 4, constants, 0);
        list->SetComputeRootShaderResourceView(1, gpu + kEdgesOffset); list->SetComputeRootShaderResourceView(2, gpu + kRangesOffset);
        list->SetComputeRootDescriptorTable(3, table);
        list->Dispatch(kCell / 8, kCell / 8, generate);
        b = Transition(atlas_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); list->ResourceBarrier(1, &b);
    }
    unsigned indices[subtitle::SubtitleTrack::kMaxActive]{}; bool whole[subtitle::SubtitleTrack::kMaxActive]{};
    const unsigned count = track_->Select(time, indices);
    unsigned shown = 0;
    for (unsigned i = 0; i < count; ++i) if ((whole[i] = Resident(indices[i], false))) { if (shown < drawn.size()) drawn[shown] = indices[i]; ++shown; }
    if (!shown) return 0;
    list->SetGraphicsRootSignature(graphics_root_); list->SetPipelineState(graphics_pipeline_);
    list->SetGraphicsRootShaderResourceView(1, gpu); list->SetGraphicsRootDescriptorTable(2, table); // The range starts at descriptor 1 (the SRV).
    const D3D12_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1}; const D3D12_RECT scissor{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor); list->OMSetRenderTargets(1,&target,FALSE,nullptr); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Layout space -> window: uniform scale, block centred horizontally; outline and shadow follow the font size
    // (1.4 px and 1.6 px at a 30 px font, the 720p size).
    const float font = FontPixels(width,height,dpi), scale = font / kLayoutEm, w = static_cast<float>(width), h = static_cast<float>(height);
    const float left = (w - kWrapEm * kLayoutEm * scale) * 0.5f, ceiling = h * 0.03f;
    float bottom = h - bottom_inset;
    auto* quads = reinterpret_cast<Quad*>(base); unsigned written = 0;
    for (unsigned i = count; i > 0; --i) {
        if (!whole[i-1]) continue;
        const auto& cue = cues_[indices[i-1]]; if (!cue.count) continue; // Markup-only cue: no ink, no space.
        const float top = bottom - cue.height * scale;
        if (top < ceiling) break; // No room left above: older cues are skipped rather than drawn off-screen.
        const unsigned first = written;
        for (unsigned g = 0; g < cue.count && written < kRegionQuads; ++g) {
            const auto& glyph = glyphs_[cue.first + g]; const auto& r = residency_[glyph.key];
            if (r.state != kResident) continue; // Spaces, and glyphs DirectWrite could not outline.
            const float size = kCell * glyph.scale / r.scale;
            quads[written++] = {glyph.pen + r.x * glyph.scale, glyph.baseline + r.y * glyph.scale, size, size,
                static_cast<float>(r.cell % kColumns * kCell), static_cast<float>(r.cell / kColumns * kCell), static_cast<float>(kCell), static_cast<float>(kCell)};
        }
        if (written > first) {
            struct Constants { float width,height,x,y; unsigned first; float scale,atlas_width,atlas_height,outline,shadow,pad0,pad1; };
            const Constants c{w,h,left,top,first,scale,static_cast<float>(atlas_width_),static_cast<float>(atlas_height_),
                              (std::max)(1.0f, font * (1.4f / 30)),font * (1.6f / 30),0,0};
            list->SetGraphicsRoot32BitConstants(0,12,&c,0); list->DrawInstanced(6,written - first,0,0);
        }
        bottom = top - font * 0.4f;
    }
    return shown;
}
bool SdfSubtitlePipeline::WaitShaped(unsigned timeout_ms) noexcept {
    if (!track_) return false;
    const auto deadline = GetTickCount64() + timeout_ms;
    while (shaped_.load(std::memory_order_acquire) < track_->Cues().size()) { if (GetTickCount64() >= deadline) return false; Sleep(1); }
    return true;
}
bool SdfSubtitlePipeline::Settle(std::int64_t time, unsigned timeout_ms) noexcept {
    if (!Ready()) return false;
    const auto deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        Update(time);
        unsigned active[subtitle::SubtitleTrack::kMaxActive]{};
        const unsigned n = track_->Select(time, active);
        bool all = true;
        for (unsigned i = 0; i < n; ++i) all = all && Resident(active[i], true);
        if (all) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
    }
}
SdfSubtitlePipeline::Statistics SdfSubtitlePipeline::Stats() const noexcept {
    Statistics s{};
    s.unique = shaping_.unique.load(std::memory_order_acquire);
    s.instances = shaping_.instances.load(std::memory_order_relaxed); s.runs = shaping_.runs.load(std::memory_order_relaxed);
    s.rtl_runs = shaping_.rtl_runs.load(std::memory_order_relaxed); s.rtl_lines = shaping_.rtl_lines.load(std::memory_order_relaxed);
    s.missing = shaping_.missing.load(std::memory_order_relaxed); s.font_faces = shaping_.faces.load(std::memory_order_relaxed);
    s.shaped = shaped_.load(std::memory_order_acquire);
    s.glyphs = resident_; s.cells = cells_; s.generated = generated_; s.evictions = evictions_;
    s.dropped = shaping_.dropped.load(std::memory_order_relaxed) + broken_;
    return s;
}
SubtitleStatus SdfSubtitlePipeline::Status() const noexcept {
    if (!track_) return SubtitleStatus::Off;
    if (gpu_state_.load(std::memory_order_acquire) == 2 || FAILED(shaper_error_.load())) return SubtitleStatus::Disabled;
    return Stats().dropped ? SubtitleStatus::AtlasExhausted : SubtitleStatus::Ready;
}
unsigned SdfSubtitlePipeline::Placements(unsigned cue, std::span<Placement> out) const noexcept {
    if (!track_ || cue >= track_->Cues().size() || !cues_[cue].ready.load(std::memory_order_acquire)) return 0;
    const auto& layout = cues_[cue]; unsigned n = 0;
    for (unsigned g = 0; g < layout.count && n < out.size(); ++g) {
        const auto& glyph = glyphs_[layout.first + g];
        if (keys_[glyph.key].empty) continue; // Spaces are not placed.
        out[n++] = {glyph.pen, glyph.baseline, kEm * glyph.scale, glyph.key};
    }
    return n;
}
float SdfSubtitlePipeline::CueHeight(unsigned cue) const noexcept {
    return track_ && cue < track_->Cues().size() && cues_[cue].ready.load(std::memory_order_acquire) ? cues_[cue].height : 0;
}
void SdfSubtitlePipeline::ReleaseGpu() noexcept {
    WaitGpu();
    if (upload_ && mapped_) upload_->Unmap(0, nullptr);
    mapped_ = nullptr;
    Drop(graphics_pipeline_); Drop(graphics_root_); Drop(compute_pipeline_); Drop(compute_root_);
    Drop(atlas_); Drop(upload_); Drop(descriptors_); Drop(cpu_descriptors_);
}
void SdfSubtitlePipeline::Release() noexcept {
    if (thread_) {
        stop_.store(true, std::memory_order_release); (void)SetEvent(wake_);
        (void)WaitForSingleObject(thread_, INFINITE); CloseHandle(thread_); thread_ = nullptr;
    }
    if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
    // The shaping thread is joined: its font faces can be released.
    if (keys_) for (unsigned i = 0, n = shaping_.unique.load(); i < n; ++i) Drop(keys_[i].face);
    ReleaseGpu();
    track_ = nullptr; keys_ = nullptr;
}
}
