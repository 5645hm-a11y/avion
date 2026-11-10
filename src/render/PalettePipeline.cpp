#include "media/render/PalettePipeline.hpp"
#include "media/text/Utf8.hpp"
#include "PaletteVertexShader.hpp"
#include "PalettePixelShader.hpp"
#include <d3dcompiler.h>
#include <dwrite_1.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iterator>
namespace media::render {
namespace {
template<class T> void Drop(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
// What the pixel shader reads: the palette's uniforms, then the text lane and raster of this composite.
struct Constants {
    ui::PaletteUniforms palette;
    float text_x, text_y, text_width, text_height;
    float caret_x, stride, travel_x, travel_y;
};
static_assert(sizeof(Constants) == 96);
// A DirectWrite renderer (text thread) that draws glyph runs into the GDI-compatible bitmap target and records
// underlines (an input method's composition) for the caller to fill.
class Painter final : public IDWriteTextRenderer {
public:
    struct Line { float x, y, width, thickness; };
    IDWriteBitmapRenderTarget* target = nullptr; IDWriteRenderingParams* params = nullptr;
    Line lines[8]{}; unsigned line_count = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
        if (!p) return E_POINTER;
        *p = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IDWriteTextRenderer) && id != __uuidof(IDWritePixelSnapping)) return E_NOINTERFACE;
        *p = static_cast<IDWriteTextRenderer*>(this); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override { *m = {1, 0, 0, 1, 0, 0}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override { *pixels = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        return target->DrawGlyphRun(x, y, mode, run, params, RGB(255, 255, 255), nullptr);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* underline, IUnknown*) override {
        // The origin is the run's leading edge: its right-hand end for right-to-left text.
        const float left = underline->readingDirection == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT ? x - underline->width : x;
        if (line_count < 8) lines[line_count++] = {left, y + underline->offset, underline->width, underline->thickness};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return E_NOTIMPL; }
};
}
PalettePipeline::Key PalettePipeline::KeyOf(const ui::CommandPalette& palette) noexcept {
    const auto lane = palette.TextRect();
    Key key; key.revision = palette.Revision(); key.font = lane.font_pixels;
    key.width = (std::min)(lane.width, kMaxWidth); key.height = (std::min)(lane.height, kMaxHeight);
    key.valid = key.width != 0 && key.height != 0 && key.font >= 1;
    return key;
}
HRESULT PalettePipeline::Prepare(ID3D12Device* device, core::LinearMemoryArena& arena) noexcept {
    if (!device || staging_ || arena.IsSealed()) return E_INVALIDARG;
    auto* text = static_cast<wchar_t*>(arena.Allocate(sizeof(wchar_t) * kMaxUnits, alignof(wchar_t)));
    auto* staging = static_cast<std::uint8_t*>(arena.Allocate(kRegionBytes));
    requests_ = arena.Create<core::SpscRingBuffer<Request, 2>>();
    completions_ = arena.Create<core::SpscRingBuffer<Completion, 2>>();
    if (!text || !staging || !requests_ || !completions_) return E_OUTOFMEMORY;
    request_text_ = text; staging_ = staging;
    HRESULT hr = CreateGpu(device);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr) && !wake_) hr = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr)) { thread_ = CreateThread(nullptr, 0, TextEntry, this, 0, nullptr); if (!thread_) hr = HRESULT_FROM_WIN32(GetLastError()); }
    if (FAILED(hr)) { Release(); return hr; }
    // Its fonts and its bitmap are created before playback can start: no such work, and no CRT per-thread setup,
    // falls into the first keystroke.
    while (!running_.load(std::memory_order_acquire)) Sleep(0);
    hr = thread_error_.load();
    if (FAILED(hr)) Release();
    return hr;
}
HRESULT PalettePipeline::CreateGpu(ID3D12Device* device) noexcept {
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 0, sizeof(Constants) / sizeof(float)};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[1].Descriptor = {0, 0};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC signature{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    Drop(blob); Drop(errors);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root_;
    p.VS = {g_PaletteVertexShader, sizeof(g_PaletteVertexShader)}; p.PS = {g_PalettePixelShader, sizeof(g_PalettePixelShader)};
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
    if (SUCCEEDED(hr)) hr = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline_));
    // The rasters the shader reads: kRegions of them, so that one can be written while the GPU still reads another.
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = std::uint64_t{kRegions} * kRegionBytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_));
    void* mapped = nullptr; const D3D12_RANGE none{0, 0};
    if (SUCCEEDED(hr)) hr = upload_->Map(0, &none, &mapped);
    if (SUCCEEDED(hr)) { mapped_ = static_cast<std::uint8_t*>(mapped); std::memset(mapped_, 0, std::size_t{kRegions} * kRegionBytes); }
    if (FAILED(hr)) ReleaseGpu();
    return hr;
}
HRESULT PalettePipeline::Recreate(ID3D12Device* device) noexcept {
    if (!device || !staging_) return E_UNEXPECTED;
    ReleaseGpu();
    return CreateGpu(device);
}
void PalettePipeline::ReleaseGpu() noexcept {
    if (upload_ && mapped_) upload_->Unmap(0, nullptr);
    mapped_ = nullptr; Drop(upload_); Drop(pipeline_); Drop(root_);
    // The raster on screen went with the buffer: the text is asked for again (a raster in flight still arrives).
    for (auto& fence : region_fence_) fence = 0;
    current_ = 0; shown_key_ = {}; shown_ = {}; if (!in_flight_) sent_ = {};
    pending_upload_ = false; staged_key_ = {};
}
void PalettePipeline::Release() noexcept {
    if (thread_) {
        stop_.store(true, std::memory_order_release); SetEvent(wake_);
        (void)WaitForSingleObject(thread_, INFINITE); CloseHandle(thread_); thread_ = nullptr;
    }
    if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
    ReleaseGpu();
    stop_.store(false); running_.store(false);
    staging_ = nullptr; request_text_ = nullptr; requests_ = nullptr; completions_ = nullptr;
    in_flight_ = false; sent_ = {};
}
void PalettePipeline::Collect() noexcept {
    if (!completions_) return;
    while (const auto* completion = completions_->PeekReadSlot()) {
        staged_ = *completion; (void)completions_->ReleaseReadSlot();
        in_flight_ = false; ++stats_.rasters;
        if (FAILED(staged_.result)) { ++stats_.failures; stats_.error = staged_.result; staged_key_ = {}; continue; }
        // Without a buffer to show it in (a device being recovered), the raster is dropped and asked for again.
        if (!mapped_) { sent_ = {}; staged_key_ = {}; continue; }
        staged_key_ = sent_; pending_upload_ = true;
    }
}
void PalettePipeline::Update(const ui::CommandPalette& palette) noexcept {
    if (!staging_) return;
    Collect();
    if (!palette.Animating()) { pending_upload_ = false; return; } // Hidden: nothing to show it in.
    const auto key = KeyOf(palette);
    if (!key.valid || Same(key, sent_) || in_flight_) return;
    // The staging raster is single: a finished one that was never drawn is superseded by the newer text.
    pending_upload_ = false; staged_key_ = {};
    // The text as UTF-16 for DirectWrite: the committed text, then the composition (underlined).
    unsigned units = 0, committed = 0;
    const auto append = [&](std::string_view text) {
        while (!text.empty()) {
            std::uint32_t scalar = 0;
            if (!text::Utf8(text, scalar) || units + 2 > kMaxUnits) break;
            if (scalar >= 0x10000) {
                scalar -= 0x10000;
                request_text_[units++] = static_cast<wchar_t>(0xd800 + (scalar >> 10)); request_text_[units++] = static_cast<wchar_t>(0xdc00 + (scalar & 0x3ff));
            } else request_text_[units++] = static_cast<wchar_t>(scalar);
        }
    };
    append(palette.Text()); committed = units; append(palette.Composition());
    auto* request = requests_->AcquireWriteSlot();
    if (!request) return;
    *request = {units, committed, key.width, key.height, key.font};
    if (!requests_->CommitWriteSlot()) return;
    sent_ = key; in_flight_ = true; ++stats_.requests;
    SetEvent(wake_);
}
bool PalettePipeline::Settle(const ui::CommandPalette& palette, unsigned timeout_ms) noexcept {
    const auto deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        Update(palette);
        const auto key = KeyOf(palette);
        if (!key.valid) return false;
        if (!in_flight_ && (Same(key, staged_key_) || Same(key, shown_key_))) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
    }
}
void PalettePipeline::Record(ID3D12GraphicsCommandList* commands, D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height,
                             const ui::CommandPalette& palette, Timeline timeline) noexcept {
    if (!pipeline_ || !mapped_) return;
    Collect();
    if (pending_upload_) {
        // Into a region no queued command list still reads: not the one on screen, and its last use retired.
        for (unsigned i = 1; i <= kRegions; ++i) {
            const unsigned region = (current_ + i) % kRegions;
            if (region == current_ && shown_key_.valid) continue;
            if (region_fence_[region] > timeline.completed) continue;
            std::memcpy(mapped_ + std::size_t{region} * kRegionBytes, staging_, std::size_t{staged_.stride} * staged_.height);
            current_ = region; shown_ = staged_; shown_key_ = staged_key_; pending_upload_ = false; ++stats_.uploads;
            stats_.width = shown_.width; stats_.height = shown_.height; stats_.caret = shown_.caret;
            stats_.ink_left = shown_.ink_left; stats_.ink_right = shown_.ink_right; stats_.right_to_left = shown_.right_to_left != 0;
            break;
        }
    }
    const auto& u = palette.Uniforms(); const auto lane = palette.TextRect();
    // A raster made for another lane size (a resize just happened) is not shown; its replacement is on its way.
    const unsigned raster_width = (std::min)(lane.width, kMaxWidth), raster_height = (std::min)(lane.height, kMaxHeight);
    const bool text = shown_key_.valid && shown_.width == raster_width && shown_.height == raster_height;
    Constants constants{};
    constants.palette = u;
    constants.text_x = static_cast<float>(lane.x); constants.text_y = static_cast<float>(lane.y + static_cast<int>((lane.height - raster_height) / 2));
    constants.text_width = static_cast<float>(raster_width); constants.text_height = static_cast<float>(raster_height);
    constants.caret_x = text ? std::round(shown_.caret) : 0.0f; constants.stride = text ? static_cast<float>(shown_.stride) : 0.0f;
    // Text whose raster is still on its way has no known end yet: no caret for that frame, rather than one at the start.
    if (!text && (!palette.Text().empty() || !palette.Composition().empty())) constants.palette.caret = 0;
    // The bar arrives from slightly above and shakes sideways when it refuses; the text travels with it, pixel for pixel.
    constants.travel_x = std::round(u.shake * u.scale);
    constants.travel_y = -std::round((1 - u.visibility) * ui::CommandPalette::kSlide * u.scale);
    if (text) region_fence_[current_] = timeline.next;
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const float horizontal = (u.half_width + kMargin) * u.scale, vertical = (u.half_height + kMargin) * u.scale;
    const D3D12_RECT scissor{
        static_cast<LONG>((std::max)(0.0f, std::floor(u.center_x - horizontal))),
        static_cast<LONG>((std::max)(0.0f, std::floor(u.center_y - vertical))),
        static_cast<LONG>((std::min)(static_cast<float>(width), std::ceil(u.center_x + horizontal))),
        static_cast<LONG>((std::min)(static_cast<float>(height), std::ceil(u.center_y + vertical)))};
    if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) return;
    commands->SetPipelineState(pipeline_); commands->SetGraphicsRootSignature(root_);
    commands->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / sizeof(float), &constants, 0);
    commands->SetGraphicsRootShaderResourceView(1, upload_->GetGPUVirtualAddress() + std::uint64_t{current_} * kRegionBytes);
    commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); commands->DrawInstanced(3, 1, 0, 0);
}
DWORD WINAPI PalettePipeline::TextEntry(void* self) noexcept { static_cast<PalettePipeline*>(self)->TextThread(); return 0; }
void PalettePipeline::TextThread() noexcept {
    errno = 0; // Creates this thread's CRT data now, before Prepare returns (and before any allocation measurement).
    IDWriteFactory* factory = nullptr; IDWriteGdiInterop* interop = nullptr; IDWriteBitmapRenderTarget* target = nullptr;
    IDWriteBitmapRenderTarget1* grey = nullptr; IDWriteRenderingParams* params = nullptr; IDWriteTextFormat* format = nullptr;
    float format_size = 0;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&factory));
    if (SUCCEEDED(hr)) hr = factory->GetGdiInterop(&interop);
    if (SUCCEEDED(hr)) hr = interop->CreateBitmapRenderTarget(nullptr, kMaxWidth, kMaxHeight, &target);
    if (SUCCEEDED(hr)) hr = target->SetPixelsPerDip(1);
    // Greyscale coverage in linear steps (gamma 1, no ClearType fringes): the shader blends it itself.
    if (SUCCEEDED(hr)) hr = target->QueryInterface(__uuidof(IDWriteBitmapRenderTarget1), reinterpret_cast<void**>(&grey));
    if (SUCCEEDED(hr)) hr = grey->SetTextAntialiasMode(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (SUCCEEDED(hr)) hr = factory->CreateCustomRenderingParams(1.0f, 0.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &params);
    const std::uint8_t* bits = nullptr; unsigned pitch = 0; bool top_down = true;
    if (SUCCEEDED(hr)) {
        DIBSECTION dib{};
        const HDC dc = target->GetMemoryDC();
        if (GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof(dib), &dib) == sizeof(dib) && dib.dsBm.bmBits && dib.dsBm.bmBitsPixel == 32) {
            bits = static_cast<const std::uint8_t*>(dib.dsBm.bmBits); pitch = static_cast<unsigned>(dib.dsBm.bmWidthBytes);
            // Which way up the bitmap's memory is, found by marking its top-left pixel (the header's sign is not
            // reported reliably for a bitmap created by someone else).
            std::memset(const_cast<std::uint8_t*>(bits), 0, std::size_t{pitch} * kMaxHeight);
            (void)SetPixel(dc, 0, 0, RGB(255, 255, 255)); (void)GdiFlush();
            top_down = bits[1] != 0;
            std::memset(const_cast<std::uint8_t*>(bits), 0, std::size_t{pitch} * kMaxHeight);
        } else hr = E_FAIL;
    }
    const auto row = [&](unsigned y) { return const_cast<std::uint8_t*>(bits) + std::size_t{top_down ? y : kMaxHeight - 1 - y} * pitch; };
    Painter painter; painter.target = target; painter.params = params;
    // A first layout loads the interface font and its fallback machinery now, not at the first keystroke.
    const auto prepare = [&](float size) {
        if (format && format_size == size) return S_OK;
        Drop(format); format_size = 0;
        HRESULT made = factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
        if (SUCCEEDED(made)) made = format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (SUCCEEDED(made)) made = format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(made)) format_size = size;
        return made;
    };
    const auto raster = [&](const Request& request) {
        Completion done{};
        const unsigned width = (std::min)(request.width, kMaxWidth), height = (std::min)(request.height, kMaxHeight);
        const unsigned units = (std::min)(request.units, kMaxUnits), committed = (std::min)(request.committed, units);
        done.width = width; done.height = height; done.stride = (width + 3) & ~3u;
        HRESULT result = prepare(request.font);
        for (unsigned y = 0; y < height && SUCCEEDED(result); ++y) std::memset(row(y), 0, std::size_t{width} * 4);
        painter.line_count = 0;
        float shift = 0;
        if (units && SUCCEEDED(result)) {
            // The line's direction is that of its first strong character, as in any text field.
            WORD types[kMaxUnits]; bool right_to_left = false;
            if (GetStringTypeW(CT_CTYPE2, request_text_, static_cast<int>(units), types))
                for (unsigned i = 0; i < units; ++i) {
                    if (types[i] == C2_RIGHTTOLEFT) { right_to_left = true; break; }
                    if (types[i] == C2_LEFTTORIGHT) break;
                }
            IDWriteTextLayout* layout = nullptr; DWRITE_TEXT_METRICS metrics{}; DWRITE_HIT_TEST_METRICS hit{}; float x = 0, y = 0;
            result = factory->CreateTextLayout(request_text_, units, format, static_cast<float>(width), static_cast<float>(height), &layout);
            if (SUCCEEDED(result) && right_to_left) result = layout->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
            if (SUCCEEDED(result) && committed < units) result = layout->SetUnderline(TRUE, {committed, units - committed});
            if (SUCCEEDED(result)) result = layout->GetMetrics(&metrics);
            if (SUCCEEDED(result)) result = layout->HitTestTextPosition(units - 1, TRUE, &x, &y, &hit);
            if (SUCCEEDED(result)) {
                // Text longer than the lane scrolls so that its end, where the caret is, stays inside.
                const float pad = (std::max)(2.0f, std::floor(request.font / 6)), lane = static_cast<float>(width);
                if (x > lane - pad) shift = std::floor(lane - pad - x); else if (x < pad && right_to_left) shift = std::ceil(pad - x);
                result = layout->Draw(nullptr, &painter, shift, 0);
                done.caret = (std::clamp)(x + shift, 0.0f, lane - 1);
                done.ink_left = metrics.left + shift; done.ink_right = metrics.left + metrics.widthIncludingTrailingWhitespace + shift;
                done.right_to_left = right_to_left ? 1u : 0u;
            }
            Drop(layout);
        }
        if (SUCCEEDED(result)) {
            // One byte of coverage per pixel (the bitmap is white on black; any channel is the coverage).
            for (unsigned y = 0; y < height; ++y) {
                const auto* source = row(y); auto* out = staging_ + std::size_t{y} * done.stride;
                for (unsigned x = 0; x < width; ++x) out[x] = source[std::size_t{x} * 4 + 1];
                for (unsigned x = width; x < done.stride; ++x) out[x] = 0;
            }
            for (unsigned i = 0; i < painter.line_count; ++i) {
                const auto& line = painter.lines[i];
                const int left = (std::max)(0, static_cast<int>(std::floor(line.x + shift))), right = (std::min)(static_cast<int>(width), static_cast<int>(std::ceil(line.x + shift + line.width)));
                const int top = (std::max)(0, static_cast<int>(std::round(line.y))), bottom = (std::min)(static_cast<int>(height), top + (std::max)(1, static_cast<int>(std::round(line.thickness))));
                for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) staging_[std::size_t(y) * done.stride + static_cast<unsigned>(x)] = 215;
            }
        }
        done.result = result;
        return done;
    };
    if (SUCCEEDED(hr)) {
        // Warm the font system with each script family the fallback covers, so their first use costs nothing later.
        static constexpr wchar_t warm[] = L"Aa \x05e9\x05dc\x05d5\x05dd \x0645\x0631\x062d\x0628\x0627 \x65e5\x672c\x8a9e \x041f\x0440\x0438\x0432\x0435\x0442";
        const unsigned units = static_cast<unsigned>(std::size(warm) - 1);
        std::memcpy(request_text_, warm, sizeof(wchar_t) * units);
        hr = raster({units, units, 640, 56, 22}).result;
    }
    thread_error_.store(hr); running_.store(true, std::memory_order_release);
    while (SUCCEEDED(hr)) {
        (void)WaitForSingleObject(wake_, INFINITE);
        if (stop_.load(std::memory_order_acquire)) break;
        while (const auto* request = requests_->PeekReadSlot()) {
            const Request copy = *request; (void)requests_->ReleaseReadSlot();
            const auto done = raster(copy);
            auto* completion = completions_->AcquireWriteSlot();
            if (completion) { *completion = done; (void)completions_->CommitWriteSlot(); }
        }
    }
    Drop(format); Drop(params); Drop(grey); Drop(target); Drop(interop); Drop(factory);
}
}
