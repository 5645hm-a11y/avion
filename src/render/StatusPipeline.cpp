#include "media/render/StatusPipeline.hpp"
#include "media/text/Utf8.hpp"
#include "StatusVertexShader.hpp"
#include "StatusPixelShader.hpp"
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
// What the pixel shader reads: the cue's uniforms, then where this composite's raster lies.
struct Constants {
    ui::StatusUniforms cue;
    float text_x, text_y, text_width, text_height;
    float stride, travel_y, reserved0, reserved1;
};
static_assert(sizeof(Constants) == 96);
// Draws glyph runs into the GDI-compatible bitmap target (text thread).
class Painter final : public IDWriteTextRenderer {
public:
    IDWriteBitmapRenderTarget* target = nullptr; IDWriteRenderingParams* params = nullptr;
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
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return E_NOTIMPL; }
};
}
StatusPipeline::Key StatusPipeline::KeyOf(const ui::StatusCue& cue) noexcept {
    const auto lane = cue.PendingRect(); // The lines of the text asked for (the panel's height changes with it).
    Key key; key.revision = cue.Revision(); key.font = lane.font_pixels;
    key.width = (std::min)(lane.width, kMaxWidth); key.height = (std::min)(lane.height, kMaxHeight);
    key.valid = key.width != 0 && key.height != 0 && key.font >= 1 && !cue.Text().empty();
    return key;
}
HRESULT StatusPipeline::Prepare(ID3D12Device* device, core::LinearMemoryArena& arena) noexcept {
    if (!device || staging_ || arena.IsSealed()) return E_INVALIDARG;
    auto* text = static_cast<wchar_t*>(arena.Allocate(sizeof(wchar_t) * (kMaxUnits + 2), alignof(wchar_t)));
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
    while (!running_.load(std::memory_order_acquire)) Sleep(0); // Fonts and bitmap exist before playback starts.
    hr = thread_error_.load();
    if (FAILED(hr)) Release();
    return hr;
}
HRESULT StatusPipeline::CreateGpu(ID3D12Device* device) noexcept {
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
    p.VS = {g_StatusVertexShader, sizeof(g_StatusVertexShader)}; p.PS = {g_StatusPixelShader, sizeof(g_StatusPixelShader)};
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
HRESULT StatusPipeline::Recreate(ID3D12Device* device) noexcept {
    if (!device || !staging_) return E_UNEXPECTED;
    ReleaseGpu();
    return CreateGpu(device);
}
void StatusPipeline::ReleaseGpu() noexcept {
    if (upload_ && mapped_) upload_->Unmap(0, nullptr);
    mapped_ = nullptr; Drop(upload_); Drop(pipeline_); Drop(root_);
    for (auto& fence : region_fence_) fence = 0;
    current_ = 0; shown_key_ = {}; shown_ = {}; if (!in_flight_) sent_ = {};
    pending_upload_ = false; staged_key_ = {};
}
void StatusPipeline::Release() noexcept {
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
void StatusPipeline::Collect(ui::StatusCue* cue, std::int64_t now) noexcept {
    if (!completions_) return;
    while (const auto* completion = completions_->PeekReadSlot()) {
        staged_ = *completion; (void)completions_->ReleaseReadSlot();
        in_flight_ = false; ++stats_.rasters;
        if (FAILED(staged_.result)) { ++stats_.failures; stats_.error = staged_.result; staged_key_ = {}; continue; }
        if (!mapped_) { sent_ = {}; staged_key_ = {}; continue; } // A device being recovered: asked for again.
        staged_key_ = sent_; pending_upload_ = true;
        if (cue) cue->Ready(staged_key_.revision, staged_.ink_right - staged_.ink_left, now);
    }
}
void StatusPipeline::Update(ui::StatusCue& cue, std::int64_t now) noexcept {
    if (!staging_) return;
    Collect(&cue, now);
    const auto key = KeyOf(cue);
    if (!key.valid || Same(key, sent_) || in_flight_) return;
    if (Same(key, shown_key_) || Same(key, staged_key_)) { sent_ = key; return; } // Already there (a lane recomputed alike).
    pending_upload_ = false; staged_key_ = {};
    unsigned units = 0;
    for (auto text = cue.Text(); !text.empty();) {
        std::uint32_t scalar = 0;
        if (!text::Utf8(text, scalar) || units + 2 > kMaxUnits) break;
        if (scalar >= 0x10000) {
            scalar -= 0x10000;
            request_text_[units++] = static_cast<wchar_t>(0xd800 + (scalar >> 10)); request_text_[units++] = static_cast<wchar_t>(0xdc00 + (scalar & 0x3ff));
        } else request_text_[units++] = static_cast<wchar_t>(scalar);
    }
    auto* request = requests_->AcquireWriteSlot();
    if (!request) return;
    *request = {units, key.width, key.height, key.font};
    if (!requests_->CommitWriteSlot()) return;
    sent_ = key; in_flight_ = true; ++stats_.requests;
    SetEvent(wake_);
}
bool StatusPipeline::Settle(ui::StatusCue& cue, std::int64_t now, unsigned timeout_ms) noexcept {
    const auto deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        Update(cue, now);
        const auto key = KeyOf(cue);
        if (!key.valid) return false;
        if (!in_flight_ && (Same(key, staged_key_) || Same(key, shown_key_))) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
    }
}
void StatusPipeline::Record(ID3D12GraphicsCommandList* commands, D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height,
                            const ui::StatusCue& cue, Timeline timeline) noexcept {
    if (!pipeline_ || !mapped_) return;
    if (pending_upload_) {
        // Into a region no queued command list still reads.
        for (unsigned i = 1; i <= kRegions; ++i) {
            const unsigned region = (current_ + i) % kRegions;
            if (region == current_ && shown_key_.valid) continue;
            if (region_fence_[region] > timeline.completed) continue;
            std::memcpy(mapped_ + std::size_t{region} * kRegionBytes, staging_, std::size_t{staged_.stride} * staged_.height);
            current_ = region; shown_ = staged_; shown_key_ = staged_key_; pending_upload_ = false; ++stats_.uploads;
            stats_.width = shown_.width; stats_.height = shown_.height; stats_.ink_left = shown_.ink_left; stats_.ink_right = shown_.ink_right;
            stats_.revision = shown_key_.revision;
            break;
        }
    }
    const auto& u = cue.Uniforms(); const auto lane = cue.TextRect();
    const unsigned raster_width = (std::min)(lane.width, kMaxWidth), raster_height = (std::min)(lane.height, kMaxHeight);
    // The text of the revision the pill shows, made for this lane (a resize asks for a new one).
    const bool text = shown_key_.valid && shown_key_.revision == cue.ShownRevision() && shown_.width == raster_width && shown_.height == raster_height;
    Constants constants{};
    constants.cue = u;
    constants.travel_y = -std::round(u.travel * u.scale);
    // The raster's first ink column at the lane's left edge.
    constants.text_x = std::round(cue.TextLeft() - (text ? shown_.ink_left : 0.0f));
    constants.text_y = static_cast<float>(lane.y + static_cast<int>((lane.height - raster_height) / 2));
    constants.text_width = static_cast<float>(raster_width); constants.text_height = static_cast<float>(raster_height);
    constants.stride = text ? static_cast<float>(shown_.stride) : 0.0f;
    if (text) region_fence_[current_] = timeline.next;
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const float horizontal = (u.half_width + kMargin) * u.scale, vertical = (u.half_height + kMargin) * u.scale;
    const D3D12_RECT scissor{
        static_cast<LONG>((std::max)(0.0f, std::floor(u.center_x - horizontal))),
        static_cast<LONG>((std::max)(0.0f, std::floor(u.center_y + constants.travel_y - vertical))),
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
DWORD WINAPI StatusPipeline::TextEntry(void* self) noexcept { static_cast<StatusPipeline*>(self)->TextThread(); return 0; }
void StatusPipeline::TextThread() noexcept {
    errno = 0; // This thread's CRT data, before Prepare returns.
    IDWriteFactory* factory = nullptr; IDWriteGdiInterop* interop = nullptr; IDWriteBitmapRenderTarget* target = nullptr;
    IDWriteBitmapRenderTarget1* grey = nullptr; IDWriteRenderingParams* params = nullptr; IDWriteTextFormat* format = nullptr;
    float format_size = 0;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&factory));
    if (SUCCEEDED(hr)) hr = factory->GetGdiInterop(&interop);
    if (SUCCEEDED(hr)) hr = interop->CreateBitmapRenderTarget(nullptr, kMaxWidth, kMaxHeight, &target);
    if (SUCCEEDED(hr)) hr = target->SetPixelsPerDip(1);
    if (SUCCEEDED(hr)) hr = target->QueryInterface(__uuidof(IDWriteBitmapRenderTarget1), reinterpret_cast<void**>(&grey));
    if (SUCCEEDED(hr)) hr = grey->SetTextAntialiasMode(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (SUCCEEDED(hr)) hr = factory->CreateCustomRenderingParams(1.0f, 0.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &params);
    const std::uint8_t* bits = nullptr; unsigned pitch = 0; bool top_down = true;
    if (SUCCEEDED(hr)) {
        DIBSECTION dib{};
        const HDC dc = target->GetMemoryDC();
        if (GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof(dib), &dib) == sizeof(dib) && dib.dsBm.bmBits && dib.dsBm.bmBitsPixel == 32) {
            bits = static_cast<const std::uint8_t*>(dib.dsBm.bmBits); pitch = static_cast<unsigned>(dib.dsBm.bmWidthBytes);
            // Which way up the memory is, by marking the top-left pixel (the header's sign is not reliable here).
            std::memset(const_cast<std::uint8_t*>(bits), 0, std::size_t{pitch} * kMaxHeight);
            (void)SetPixel(dc, 0, 0, RGB(255, 255, 255)); (void)GdiFlush();
            top_down = bits[1] != 0;
            std::memset(const_cast<std::uint8_t*>(bits), 0, std::size_t{pitch} * kMaxHeight);
        } else hr = E_FAIL;
    }
    const auto row = [&](unsigned y) { return const_cast<std::uint8_t*>(bits) + std::size_t{top_down ? y : kMaxHeight - 1 - y} * pitch; };
    Painter painter; painter.target = target; painter.params = params;
    const auto prepare = [&](float size) {
        if (format && format_size == size) return S_OK;
        Drop(format); format_size = 0;
        HRESULT made = factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
        if (SUCCEEDED(made)) made = format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (SUCCEEDED(made)) made = format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(made)) format_size = size;
        return made;
    };
    const auto raster = [&](const Request& request) {
        Completion done{};
        const unsigned width = (std::min)(request.width, kMaxWidth), height = (std::min)(request.height, kMaxHeight);
        const unsigned units = (std::min)(request.units, kMaxUnits);
        done.width = width; done.height = height; done.stride = (width + 3) & ~3u;
        HRESULT result = prepare(request.font);
        for (unsigned y = 0; y < height && SUCCEEDED(result); ++y) std::memset(row(y), 0, std::size_t{width} * 4);
        if (units && SUCCEEDED(result)) {
            // A cue reads in the direction of its first strong character.
            WORD types[kMaxUnits]; bool right_to_left = false;
            if (GetStringTypeW(CT_CTYPE2, request_text_, static_cast<int>(units), types))
                for (unsigned i = 0; i < units; ++i) {
                    if (types[i] == C2_RIGHTTOLEFT) { right_to_left = true; break; }
                    if (types[i] == C2_LEFTTORIGHT) break;
                }
            IDWriteTextLayout* layout = nullptr; DWRITE_TEXT_METRICS metrics{};
            result = factory->CreateTextLayout(request_text_, units, format, static_cast<float>(width), static_cast<float>(height), &layout);
            if (SUCCEEDED(result) && right_to_left) result = layout->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
            if (SUCCEEDED(result)) result = layout->GetMetrics(&metrics);
            if (SUCCEEDED(result)) {
                result = layout->Draw(nullptr, &painter, 0, 0);
                done.ink_left = std::floor((std::max)(metrics.left, 0.0f));
                done.ink_right = std::ceil((std::min)(metrics.left + metrics.width, static_cast<float>(width)));
            }
            Drop(layout);
        }
        if (SUCCEEDED(result))
            for (unsigned y = 0; y < height; ++y) { // One byte of coverage per pixel (white on black: any channel).
                const auto* source = row(y); auto* out = staging_ + std::size_t{y} * done.stride;
                for (unsigned x = 0; x < width; ++x) out[x] = source[std::size_t{x} * 4 + 1];
                for (unsigned x = width; x < done.stride; ++x) out[x] = 0;
            }
        done.result = result;
        return done;
    };
    if (SUCCEEDED(hr)) {
        // Warm the font system with the scripts the cues use, so their first use costs nothing later.
        static constexpr wchar_t warm[] = L"Audio \x00b7 \x05e2\x05d1\x05e8\x05d9\x05ea \x0627\x0644\x0639\x0631\x0628\x064a\x0629 \x65e5\x672c\x8a9e \x0420\x0443\x0441";
        const unsigned units = static_cast<unsigned>(std::size(warm) - 1);
        std::memcpy(request_text_, warm, sizeof(wchar_t) * units);
        hr = raster({units, 512, 40, 16}).result;
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
