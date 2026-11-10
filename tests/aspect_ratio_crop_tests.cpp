// Crop exactness and display shape of the production picture path, on synthetic NV12 (test input, never decoded video).
// - Viewport arithmetic: sample aspect ratios, display sizes, letterboxing (picture_geometry_checks.hpp).
// - No edge bleeding: a picture whose crop has odd origins and odd sizes inside a 112x80 coded surface is converted
//   with the padding outside the crop set to three different poisons. Every conversion mode at every scale must give
//   bit-identical pictures whatever the padding holds: nothing outside the crop may reach the output.
// - Exact boundaries: the same picture at an even offset in a poisoned surface and alone in a surface of its own
//   size converts to bit-identical output: the crop's edges are treated exactly as a texture's edges are.
// - Anamorphic mapping: a crop shown at its display size (SAR 4:3) maps its first and last columns to the first and
//   last output columns, and an edge in the source to the proportional output column.
// - Scaled presentation (PictureBlit.hlsl): a picture in the corner of a larger slot is scaled without blending in
//   what lies beyond it in the slot.
#include "media/render/ColorPipeline.hpp"
#include "media/render/NeuralUpscalePass.hpp"
#include "media/render/NeuralWeights.hpp"
#include "media/render/PictureBlit.hpp"
#include "BlitVertexShader.hpp"
#include "BlitPixelShader.hpp"
#include <dxgi1_6.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 40) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
}
#include "picture_geometry_checks.hpp"
namespace {
template <class T> void ReleaseCom(T*& p) { if (p) { p->Release(); p = nullptr; } }
using media::render::ColorConstants;
using Pixels = std::vector<std::uint8_t>;
constexpr unsigned CW = 112, CH = 80; // Coded surface: whole macroblocks.
constexpr unsigned kOut = 512;        // Output texture side; pictures are written into its top-left corner.
struct Crop { unsigned left, top, right, bottom; unsigned Width() const { return right - left; } unsigned Height() const { return bottom - top; } };

struct Gpu {
    ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    std::uint64_t value = 0;
    bool Submit() {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence, ++value))) return false;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool waited = SUCCEEDED(fence->SetEventOnCompletion(value, event)) && WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
        CloseHandle(event);
        return waited && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr));
    }
};
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to}; return b;
}
ID3D12Resource* Buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource))) ? resource : nullptr;
}
ID3D12Resource* Texture(ID3D12Device* device, unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = w; desc.Height = h;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = flags;
    ID3D12Resource* resource = nullptr;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource))) ? resource : nullptr;
}
// Uploads tightly packed planes (or one RGBA8 plane) into a texture in COPY_DEST; leaves it in `after`.
bool Upload(Gpu& gpu, ID3D12Resource* texture, const std::uint8_t* const* planes, const unsigned* row_bytes, unsigned plane_count,
            D3D12_RESOURCE_STATES after, std::vector<ID3D12Resource*>& keep) {
    const auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2]{}; UINT rows[2]{}; UINT64 sizes[2]{}, total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, plane_count, 0, layouts, rows, sizes, &total);
    auto* staging = Buffer(gpu.device, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!staging) return false;
    keep.push_back(staging);
    std::uint8_t* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    for (unsigned p = 0; p < plane_count; ++p)
        for (UINT r = 0; r < rows[p]; ++r)
            std::memcpy(mapped + layouts[p].Offset + static_cast<std::size_t>(r) * layouts[p].Footprint.RowPitch, planes[p] + static_cast<std::size_t>(r) * row_bytes[p], row_bytes[p]);
    staging->Unmap(0, nullptr);
    for (unsigned p = 0; p < plane_count; ++p) {
        D3D12_TEXTURE_COPY_LOCATION to{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; to.SubresourceIndex = p;
        D3D12_TEXTURE_COPY_LOCATION from{staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = layouts[p];
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    const auto barrier = Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, after);
    gpu.list->ResourceBarrier(1, &barrier);
    return true;
}
// The picture, as a function of position relative to the crop: steep ramps and steps in luma that reach every edge,
// and chroma that changes from texel to texel, so a wrong neighbour at a border changes the output.
std::uint8_t PictureLuma(unsigned x, unsigned y) { return static_cast<std::uint8_t>(20 + ((x * 5 + y * 3) % 97) * 2); }
void PictureChroma(unsigned x, unsigned y, std::uint8_t* c) {
    c[0] = static_cast<std::uint8_t>(60 + (x * 11 + y * 5) % 130); c[1] = static_cast<std::uint8_t>(200 - (x * 3 + y * 17) % 140);
}
// A coded surface holding the picture in `crop`; everything outside it is `poison`: 0 white luma with extreme chroma,
// 1 black luma with the opposite chroma, 2 noise.
void Surface(const Crop& crop, unsigned poison, unsigned coded_width, unsigned coded_height, Pixels& luma, Pixels& chroma) {
    luma.assign(static_cast<std::size_t>(coded_width) * coded_height, 0); chroma.assign(static_cast<std::size_t>(coded_width) * coded_height / 2, 0);
    std::uint32_t state = 99991u * (poison + 1);
    const auto noise = [&] { state = state * 1664525u + 1013904223u; return static_cast<std::uint8_t>(state >> 24); };
    for (unsigned y = 0; y < coded_height; ++y) for (unsigned x = 0; x < coded_width; ++x) {
        const bool inside = x >= crop.left && x < crop.right && y >= crop.top && y < crop.bottom;
        const auto n = noise();
        luma[static_cast<std::size_t>(y) * coded_width + x] = inside ? PictureLuma(x - crop.left, y - crop.top) : poison == 0 ? 255 : poison == 1 ? 0 : n;
    }
    const auto bounds = media::video::ChromaBounds(crop.left, crop.top, crop.right, crop.bottom);
    for (unsigned y = 0; y < coded_height / 2; ++y) for (unsigned x = 0; x < coded_width / 2; ++x) {
        auto* c = chroma.data() + (static_cast<std::size_t>(y) * (coded_width / 2) + x) * 2;
        const bool inside = static_cast<int>(x) >= bounds.low_x && static_cast<int>(x) <= bounds.high_x && static_cast<int>(y) >= bounds.low_y && static_cast<int>(y) <= bounds.high_y;
        const auto n0 = noise(), n1 = noise();
        if (inside) PictureChroma(x - static_cast<unsigned>(bounds.low_x), y - static_cast<unsigned>(bounds.low_y), c);
        else { c[0] = poison == 0 ? 255 : poison == 1 ? 0 : n0; c[1] = poison == 0 ? 0 : poison == 1 ? 255 : n1; }
    }
}
int MaxDifference(const Pixels& a, const Pixels& b) {
    if (a.size() != b.size()) return 255;
    int worst = 0; for (std::size_t i = 0; i < a.size(); ++i) worst = (std::max)(worst, std::abs(int(a[i]) - int(b[i]))); return worst;
}
struct Mode { const char* name; std::uint32_t reconstruction, chroma; float sharpness; std::uint32_t transfer; };
constexpr Mode kModes[]{
    {"bilinear", media::render::kBilinear, media::render::kBilinearChroma, 0, 1},
    {"bilinear, guided chroma", media::render::kBilinear, media::render::kGuidedChroma, 0, 1},
    {"edge-directed", media::render::kEdgeDirected, media::render::kBilinearChroma, 0, 1},
    {"edge-directed, guided chroma", media::render::kEdgeDirected, media::render::kGuidedChroma, 0, 1},
    {"edge-directed, guided, sharpened", media::render::kEdgeDirected, media::render::kGuidedChroma, 0.5f, 1},
    {"neural, guided chroma", media::render::kNeural, media::render::kGuidedChroma, 0, 1},
    {"HDR tone map, edge-directed, guided", media::render::kEdgeDirected, media::render::kGuidedChroma, 0, 16}};
constexpr unsigned kModeCount = sizeof(kModes) / sizeof(kModes[0]);
}

int main() {
    geometry_checks::ViewportMath();
    Gpu gpu;
    IDXGIFactory6* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        for (UINT i = 0; !gpu.device; ++i) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                (void)D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device));
            ReleaseCom(adapter);
        }
    }
    ReleaseCom(factory);
    if (!gpu.device) {
        std::printf("Viewport arithmetic: %d failures\n", failures);
        std::puts("SKIP no hardware D3D12 device"); return failures ? 1 : 77;
    }
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = gpu.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&gpu.queue));
    if (SUCCEEDED(hr)) hr = gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    if (SUCCEEDED(hr)) hr = gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    if (SUCCEEDED(hr)) hr = gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator, nullptr, IID_PPV_ARGS(&gpu.list));
    media::render::ColorPipeline color;
    if (SUCCEEDED(hr)) hr = color.Create(gpu.device);
    media::render::NeuralUpscalePass neural;
    if (SUCCEEDED(hr)) hr = neural.Create(gpu.device, gpu.queue, media::render::kNeuralWeights, 1920, 1080);
    // Descriptors: source planes of up to four surfaces at [0..7], output UAV [8], neural 2x luma UAV/SRV [9]/[10],
    // blit source SRV [11].
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 12, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (SUCCEEDED(hr)) hr = gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    auto* output = Texture(gpu.device, kOut, kOut, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* doubled = Texture(gpu.device, 2 * CW, 2 * CH, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* target = Texture(gpu.device, kOut, kOut, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (FAILED(hr) || !heap || !output || !doubled || !target) { std::printf("FAIL setup %08lx\n", static_cast<unsigned long>(hr)); return 1; }
    const auto output_desc = output->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 footprint_bytes = 0;
    gpu.device->GetCopyableFootprints(&output_desc, 0, 1, 0, &footprint, nullptr, nullptr, &footprint_bytes);
    auto* readback = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, footprint_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(readback != nullptr);
    const auto stride = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](unsigned i) { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * stride; return h; };
    auto gpu_handle = [&](unsigned i) { auto h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(i) * stride; return h; };
    gpu.device->CreateUnorderedAccessView(output, nullptr, nullptr, cpu(8));
    gpu.device->CreateUnorderedAccessView(doubled, nullptr, nullptr, cpu(9)); gpu.device->CreateShaderResourceView(doubled, nullptr, cpu(10));
    ID3D12DescriptorHeap* heaps[]{heap};
    std::vector<ID3D12Resource*> staging;
    ID3D12Resource* surfaces[4]{};
    // (Re)creates surface `slot` with the given planes and binds its two plane views.
    auto make_surface = [&](unsigned slot, unsigned w, unsigned h, const Pixels& luma, const Pixels& chroma) {
        ReleaseCom(surfaces[slot]);
        surfaces[slot] = Texture(gpu.device, w, h, DXGI_FORMAT_NV12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
        if (!surfaces[slot]) return false;
        const std::uint8_t* planes[2]{luma.data(), chroma.data()}; const unsigned rows[2]{w, w};
        if (!Upload(gpu, surfaces[slot], planes, rows, 2, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, staging)) return false;
        for (unsigned plane = 0; plane < 2; ++plane) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
            gpu.device->CreateShaderResourceView(surfaces[slot], &srv, cpu(slot * 2 + plane));
        }
        return true;
    };
    auto read_output = [&](ID3D12Resource* texture, D3D12_RESOURCE_STATES state, unsigned w, unsigned h, Pixels& out) {
        auto barrier = Transition(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION to{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        gpu.list->ResourceBarrier(1, &barrier);
        CHECK(gpu.Submit());
        out.assign(static_cast<std::size_t>(w) * h * 4, 0);
        std::uint8_t* mapped = nullptr; const D3D12_RANGE range{0, static_cast<SIZE_T>(footprint_bytes)};
        if (SUCCEEDED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
            for (unsigned y = 0; y < h; ++y)
                std::memcpy(out.data() + static_cast<std::size_t>(y) * w * 4, mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch, static_cast<std::size_t>(w) * 4);
            const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
        }
    };
    // Converts `crop` of surface `slot` (coded w x h) to out_w x out_h in `mode`, as the renderer records it.
    auto convert = [&](unsigned slot, unsigned coded_w, unsigned coded_h, const Crop& crop, unsigned out_w, unsigned out_h, const Mode& mode, Pixels& out) {
        const bool hdr = mode.transfer == 16;
        const ColorConstants k{out_w, out_h, coded_w, coded_h, crop.left, crop.top, crop.right, crop.bottom, hdr ? 9u : 1u, 0, 0, hdr ? 9u : 1u,
                               mode.transfer, 1, 1000.0f, 203.0f, mode.reconstruction, mode.sharpness, 0.02f, mode.chroma};
        gpu.list->SetDescriptorHeaps(1, heaps);
        if (mode.reconstruction == media::render::kNeural) {
            auto b = Transition(doubled, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); gpu.list->ResourceBarrier(1, &b);
            neural.Invalidate(); // Each case its own picture: the network alone, no temporal reuse.
            neural.Record(gpu.list, gpu_handle(slot * 2), gpu_handle(9), crop.left, crop.top, crop.right, crop.bottom);
            b = Transition(doubled, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); gpu.list->ResourceBarrier(1, &b);
            color.Record(gpu.list, gpu_handle(slot * 2), gpu_handle(8), k, gpu_handle(10));
            b = Transition(doubled, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON); gpu.list->ResourceBarrier(1, &b);
        } else color.Record(gpu.list, gpu_handle(slot * 2), gpu_handle(8), k);
        read_output(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out_w, out_h, out);
    };
    struct Size { unsigned w, h; const char* name; };
    const auto sizes_for = [](const Crop& crop, Size* sizes) {
        const unsigned w = crop.Width(), h = crop.Height();
        const auto anamorphic = media::video::DisplaySize(w, h, {4, 3});
        unsigned n = 0;
        sizes[n++] = {w, h, "1:1"}; sizes[n++] = {w * 3 / 2, h * 3 / 2, "1.5x"}; sizes[n++] = {2 * w, 2 * h, "2x"}; sizes[n++] = {3 * w, 3 * h, "3x"};
        sizes[n++] = {anamorphic.width, anamorphic.height, "SAR 4:3"}; sizes[n++] = {w * 7 / 5, h * 9 / 5, "1.4x by 1.8x"};
        sizes[n++] = {(w + 1) / 2, (h + 1) / 2, "1/2"};
        return n;
    };
    // ---- No edge bleeding: the output does not depend on what lies outside the crop ----
    constexpr Crop crops[]{{3, 5, 104, 72}, {8, 6, 107, 71}, {1, 1, 111, 79}, {4, 6, 104, 72}, {0, 0, 112, 80}};
    int worst_by_mode[kModeCount]{}; unsigned conversions = 0;
    Pixels luma, chroma, reference, candidate;
    for (const auto& crop : crops) {
        for (unsigned poison = 0; poison < 3; ++poison) { Surface(crop, poison, CW, CH, luma, chroma); CHECK(make_surface(poison, CW, CH, luma, chroma)); }
        CHECK(gpu.Submit());
        for (auto* s : staging) ReleaseCom(s);
        staging.clear();
        Size sizes[8]; const unsigned size_count = sizes_for(crop, sizes);
        for (unsigned m = 0; m < kModeCount; ++m) for (unsigned s = 0; s < size_count; ++s) {
            convert(0, CW, CH, crop, sizes[s].w, sizes[s].h, kModes[m], reference);
            for (unsigned poison = 1; poison < 3; ++poison) {
                convert(poison, CW, CH, crop, sizes[s].w, sizes[s].h, kModes[m], candidate);
                const int difference = MaxDifference(reference, candidate);
                if (difference > worst_by_mode[m]) {
                    worst_by_mode[m] = difference;
                    std::printf("  padding reached the picture: %s, crop (%u,%u)-(%u,%u) at %s: up to %d of 255\n", kModes[m].name, crop.left, crop.top, crop.right, crop.bottom, sizes[s].name, difference);
                }
                ++conversions;
            }
        }
    }
    std::printf("Edge bleeding, %u conversions compared against a differently poisoned surface (largest difference, 8-bit steps):\n", conversions);
    for (unsigned m = 0; m < kModeCount; ++m) { std::printf("  %-38s %d\n", kModes[m].name, worst_by_mode[m]); CHECK(worst_by_mode[m] == 0); }
    // ---- Exact boundaries: a crop's edges behave as a texture's edges ----
    {
        const Crop inside{4, 6, 104, 72}, whole{0, 0, 100, 66};
        Surface(inside, 2, CW, CH, luma, chroma); CHECK(make_surface(0, CW, CH, luma, chroma));
        Surface(whole, 0, 100, 66, luma, chroma); CHECK(make_surface(3, 100, 66, luma, chroma));
        CHECK(gpu.Submit());
        for (auto* s : staging) ReleaseCom(s);
        staging.clear();
        Size sizes[8]; const unsigned size_count = sizes_for(inside, sizes);
        int worst = 0;
        for (unsigned m = 0; m < kModeCount; ++m) for (unsigned s = 0; s < size_count; ++s) {
            convert(0, CW, CH, inside, sizes[s].w, sizes[s].h, kModes[m], reference);
            convert(3, 100, 66, whole, sizes[s].w, sizes[s].h, kModes[m], candidate);
            const int difference = MaxDifference(reference, candidate);
            if (difference > worst) { worst = difference; std::printf("  crop differs from the same picture alone: %s at %s: up to %d of 255\n", kModes[m].name, sizes[s].name, difference); }
        }
        std::printf("Crop inside a poisoned surface against the same picture in a surface of its own size: largest difference %d\n", worst);
        CHECK(worst == 0);
    }
    // ---- Anamorphic: the crop spans the whole display width, and positions scale by the sample aspect ratio ----
    {
        const Crop crop{3, 5, 104, 72}; const unsigned w = crop.Width(), h = crop.Height(), edge = 50; // Dark columns 0..49, bright from 50.
        Pixels flat_luma(static_cast<std::size_t>(CW) * CH, 255), flat_chroma(static_cast<std::size_t>(CW) * CH / 2, 128);
        for (unsigned y = crop.top; y < crop.bottom; ++y) for (unsigned x = crop.left; x < crop.right; ++x)
            flat_luma[static_cast<std::size_t>(y) * CW + x] = x - crop.left < edge ? 40 : 200;
        for (unsigned y = 0; y < crop.top; ++y) for (unsigned x = 0; x < CW; ++x) flat_luma[static_cast<std::size_t>(y) * CW + x] = 0;
        CHECK(make_surface(0, CW, CH, flat_luma, flat_chroma)); CHECK(gpu.Submit());
        for (auto* s : staging) ReleaseCom(s);
        staging.clear();
        const auto shown = media::video::DisplaySize(w, h, {4, 3});
        const auto view = media::video::Viewport(shown, {480, 270});
        CHECK(shown.width == 135 && shown.height == 67 && view.width == 480 && view.height == 238 && view.x == 0 && view.y == 16);
        for (const auto& size : {Size{shown.width, shown.height, "display size"}, Size{view.width, view.height, "viewport in 480x270"}}) {
            convert(0, CW, CH, crop, size.w, size.h, kModes[0], reference);
            const auto green = [&](unsigned x, unsigned y) { return int(reference[(static_cast<std::size_t>(y) * size.w + x) * 4 + 1]); };
            const int dark = green(0, size.h / 2), bright = green(size.w - 1, size.h / 2);
            // Limited-range 40 and 200 through BT.709 with neutral chroma: (40 - 16) / 219 and (200 - 16) / 219 of 255.
            if (std::abs(dark - 28) > 1 || std::abs(bright - 214) > 1) std::printf("  levels: dark %d, bright %d\n", dark, bright);
            CHECK(std::abs(dark - 28) <= 1 && std::abs(bright - 214) <= 1);
            unsigned wrong_rows = 0; double crossing_sum = 0;
            for (unsigned y = 0; y < size.h; ++y) {
                // Every row is the same picture row pattern: dark, one transition, bright; first and last pixels untouched.
                if (green(0, y) != dark || green(size.w - 1, y) != bright) ++wrong_rows;
                double crossing = -1; const double half = 0.5 * (dark + bright);
                for (unsigned x = 1; x < size.w; ++x) {
                    const int a = green(x - 1, y), b = green(x, y);
                    if (a < half && b >= half) { crossing = (x - 1) + (half - a) / double(b - a); break; }
                }
                if (crossing < 0) ++wrong_rows;
                crossing_sum += crossing;
            }
            // Source column boundary 50 lies at 50 / 101 of the width; output pixel centres are at integer + 0.5.
            const double expected = static_cast<double>(edge) * size.w / w - 0.5, measured = crossing_sum / size.h;
            std::printf("Anamorphic %ux%u -> %ux%u (%s): edge at output column %.2f, expected %.2f; %u rows off\n", w, h, size.w, size.h, size.name, measured, expected, wrong_rows);
            CHECK(wrong_rows == 0 && std::abs(measured - expected) < 0.75);
        }
    }
    // ---- Scaled presentation: PictureBlit.hlsl with the renderer's constants ----
    {
        ID3D12RootSignature* root = nullptr; ID3D12PipelineState* blit = nullptr; ID3D12DescriptorHeap* rtvs = nullptr;
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 0, sizeof(media::render::BlitConstants) / 4};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable = {1, &range};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC signature{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *blob = nullptr, *errors = nullptr;
        hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (SUCCEEDED(hr)) hr = gpu.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        ReleaseCom(blob); ReleaseCom(errors);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root;
        p.VS = {g_BlitVertexShader, sizeof(g_BlitVertexShader)}; p.PS = {g_BlitPixelShader, sizeof(g_BlitPixelShader)};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
        p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1; p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; p.SampleDesc.Count = 1;
        if (SUCCEEDED(hr)) hr = gpu.device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&blit));
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (SUCCEEDED(hr)) hr = gpu.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs));
        CHECK(SUCCEEDED(hr));
        if (SUCCEEDED(hr)) {
            const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
            gpu.device->CreateRenderTargetView(target, nullptr, rtv);
            constexpr unsigned kSlot = 256, PW = 101, PH = 67; // A picture in the corner of a larger slot.
            ID3D12Resource* slots[2]{};
            Pixels slot_pixels[2];
            for (unsigned poison = 0; poison < 2; ++poison) {
                auto& px = slot_pixels[poison]; px.assign(static_cast<std::size_t>(kSlot) * kSlot * 4, 0);
                for (unsigned y = 0; y < kSlot; ++y) for (unsigned x = 0; x < kSlot; ++x) {
                    auto* v = px.data() + (static_cast<std::size_t>(y) * kSlot + x) * 4;
                    if (x < PW && y < PH) { v[0] = PictureLuma(x, y); v[1] = static_cast<std::uint8_t>(255 - PictureLuma(y, x)); v[2] = static_cast<std::uint8_t>((x * 9 + y * 4) & 255); }
                    else { v[0] = poison ? 0 : 255; v[1] = poison ? 255 : 0; v[2] = poison ? 0 : 255; } // What an earlier, larger picture left.
                    v[3] = 255;
                }
                slots[poison] = Texture(gpu.device, kSlot, kSlot, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
                const std::uint8_t* planes[1]{px.data()}; const unsigned rows[1]{kSlot * 4};
                CHECK(slots[poison] && Upload(gpu, slots[poison], planes, rows, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, staging));
            }
            CHECK(gpu.Submit());
            for (auto* s : staging) ReleaseCom(s);
            staging.clear();
            auto draw_with = [&](unsigned poison, const media::render::BlitConstants& constants, unsigned w, unsigned h, Pixels& out) {
                gpu.device->CreateShaderResourceView(slots[poison], nullptr, cpu(11));
                gpu.list->SetDescriptorHeaps(1, heaps);
                gpu.list->SetGraphicsRootSignature(root); gpu.list->SetPipelineState(blit);
                gpu.list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
                gpu.list->SetGraphicsRootDescriptorTable(1, gpu_handle(11));
                const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
                const D3D12_RECT scissor{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
                gpu.list->RSSetViewports(1, &viewport); gpu.list->RSSetScissorRects(1, &scissor);
                gpu.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr); gpu.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                gpu.list->DrawInstanced(3, 1, 0, 0);
                read_output(target, D3D12_RESOURCE_STATE_RENDER_TARGET, w, h, out);
            };
            auto draw = [&](unsigned poison, unsigned w, unsigned h, Pixels& out) { draw_with(poison, media::render::MakeBlitConstants(PW, PH, kSlot, kSlot), w, h, out); };
            int worst = 0;
            for (const auto& size : {Size{PW, PH, "1:1"}, Size{303, 201, "3x"}, Size{135, 67, "SAR 4:3"}, Size{161, 107, "1.6x"}, Size{50, 33, "1/2"}, Size{512, 340, "5x"}}) {
                draw(0, size.w, size.h, reference); draw(1, size.w, size.h, candidate);
                const int difference = MaxDifference(reference, candidate);
                if (difference > worst) { worst = difference; std::printf("  the slot beyond the picture reached the screen at %s: up to %d of 255\n", size.name, difference); }
                // While enlarging, the corner pixels are the picture's corner texels exactly.
                if (size.w >= PW && size.h >= PH) {
                    const auto* corner = reference.data() + (static_cast<std::size_t>(size.h - 1) * size.w + (size.w - 1)) * 4;
                    const auto* texel = slot_pixels[0].data() + (static_cast<std::size_t>(PH - 1) * kSlot + (PW - 1)) * 4;
                    CHECK(std::abs(int(corner[0]) - int(texel[0])) <= 1 && std::abs(int(corner[1]) - int(texel[1])) <= 1 && std::abs(int(corner[2]) - int(texel[2])) <= 1);
                }
            }
            std::printf("Scaled presentation of a %ux%u picture in a %ux%u slot: largest difference between two slot contents %d\n", PW, PH, kSlot, kSlot, worst);
            CHECK(worst == 0);
            // ---- Live zoom (docs/picture.md): a picture converted from one region shown as another ----
            // The view is sampled out of the picture where the region asked for lies in the one it was converted from: here
            // the picture is its region one to one (PW x PH samples), and the reference is the bilinear sample of the picture
            // at that point, computed here. A view reaching beyond the picture is black there, never the slot's poison.
            {
                using media::render::SampleRect;
                const auto texel = [&](int x, int y, unsigned c) {
                    x = x < 0 ? 0 : x >= static_cast<int>(PW) ? static_cast<int>(PW) - 1 : x; y = y < 0 ? 0 : y >= static_cast<int>(PH) ? static_cast<int>(PH) - 1 : y;
                    return static_cast<double>(slot_pixels[0][(static_cast<std::size_t>(y) * kSlot + static_cast<unsigned>(x)) * 4 + c]);
                };
                const auto bilinear = [&](double px, double py, unsigned c) { // Picture pixels; texel centres at +0.5.
                    const double fx = px - 0.5, fy = py - 0.5, x0 = std::floor(fx), y0 = std::floor(fy), ax = fx - x0, ay = fy - y0;
                    const int ix = static_cast<int>(x0), iy = static_cast<int>(y0);
                    return (texel(ix, iy, c) * (1 - ax) + texel(ix + 1, iy, c) * ax) * (1 - ay) + (texel(ix, iy + 1, c) * (1 - ax) + texel(ix + 1, iy + 1, c) * ax) * ay;
                };
                struct View { SampleRect converted, wanted; unsigned w, h; const char* name; };
                const View views[]{{{0, 0, PW, PH}, {24, 16, 74, 50}, 200, 136, "zoom in (2x, off centre)"},
                                   {{0, 0, PW, PH}, {30, 20, 70, 44}, 101, 67, "pan"},
                                   {{25, 17, 75, 50}, {0, 0, 100, 66}, 101, 67, "zoom out (2x)"}};
                for (const auto& v : views) {
                    auto constants = media::render::MakeBlitConstants(PW, PH, kSlot, kSlot);
                    media::render::ViewOf(v.converted, v.wanted, constants);
                    draw_with(1, constants, v.w, v.h, candidate); // The slot's poison beyond the picture: must never show.
                    int error = 0; unsigned inside = 0, black = 0, outside = 0;
                    for (unsigned y = 0; y < v.h; ++y)
                        for (unsigned x = 0; x < v.w; ++x) {
                            const double qx = constants.origin[0] + (x + 0.5) / v.w * constants.span[0], qy = constants.origin[1] + (y + 0.5) / v.h * constants.span[1];
                            const auto* got = candidate.data() + (static_cast<std::size_t>(y) * v.w + x) * 4;
                            const double margin_x = 1.0 / v.w * constants.span[0], margin_y = 1.0 / v.h * constants.span[1];
                            if (qx < -margin_x || qx > 1 + margin_x || qy < -margin_y || qy > 1 + margin_y) { ++outside; black += got[0] == 0 && got[1] == 0 && got[2] == 0 ? 1u : 0u; continue; }
                            if (qx < margin_x || qx > 1 - margin_x || qy < margin_y || qy > 1 - margin_y) continue; // The edge itself.
                            ++inside;
                            for (unsigned c = 0; c < 3; ++c) error = (std::max)(error, static_cast<int>(std::lround(std::abs(bilinear(qx * PW, qy * PH, c) - got[c]))));
                        }
                    std::printf("Live zoom view, %s: origin (%.3f, %.3f) span (%.3f, %.3f); %u pixels within %d of the picture's bilinear sample, %u of %u beyond it black\n",
                                v.name, constants.origin[0], constants.origin[1], constants.span[0], constants.span[1], inside, error, black, outside);
                    CHECK(inside > v.w * v.h / 5 && error <= 3 && black == outside);
                }
                // The same region: the whole picture (the view the scaled presentation above draws).
                auto same = media::render::MakeBlitConstants(PW, PH, kSlot, kSlot);
                media::render::ViewOf({3, 5, 53, 38}, {3, 5, 53, 38}, same);
                CHECK(same.origin[0] == 0 && same.origin[1] == 0 && same.span[0] == 1 && same.span[1] == 1);
            }
            for (auto*& s : slots) ReleaseCom(s);
        }
        ReleaseCom(rtvs); ReleaseCom(blit); ReleaseCom(root);
    }
    ReleaseCom(readback); ReleaseCom(target); ReleaseCom(doubled); ReleaseCom(output); neural.Release();
    for (auto*& s : surfaces) ReleaseCom(s);
    ReleaseCom(heap); color.Release();
    ReleaseCom(gpu.list); ReleaseCom(gpu.allocator); ReleaseCom(gpu.fence); ReleaseCom(gpu.queue); ReleaseCom(gpu.device);
    std::printf("Aspect ratio and crop: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
