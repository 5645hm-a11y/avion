// Spatial stage of the production colour pipeline (Reconstruct.hlsli): edge-directed upscaling, contrast-adaptive
// sharpening and luma-guided chroma, measured on analytic fixtures (synthetic NV12 test input, never decoded video).
// Luma results come back as scalars from EdgeMetrics.hlsl; chroma fixtures are read back as RGB and inverted to
// Cb/Cr (BT.709, a pure matrix: no transfer change); plus GPU timestamps for a 720p -> 1080p conversion.
#include "media/render/ColorPipeline.hpp"
#include "media/render/NeuralUpscalePass.hpp"
#include "media/render/NeuralWeights.hpp"
#include "EdgeMetricsShader.hpp"
#include "allocation_probe.hpp"
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
template <class T> void ReleaseCom(T*& p) { if (p) { p->Release(); p = nullptr; } }
using media::render::ColorConstants;
constexpr unsigned S = 96, Up = 3, O = S * Up;               // 96 x 96 source, 3x upscale to 288 x 288.
constexpr unsigned PW = 1280, PH = 720, DW = 1920, DH = 1080;  // Timing: 720p -> 1080p.
constexpr unsigned kDark = 40, kBright = 200, kFlat = 110;     // 8-bit limited-range luma codes.
constexpr double kCenter = S / 2.0, kSlope = 0.36397023426620234; // Edge through the centre, 20 degrees from vertical.
double Encoded(unsigned code) { return (code - 16.0) / 219.0; }

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
ID3D12Resource* Buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
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
// NV12 upload of a luma image with neutral chroma, or with `chroma` (interleaved Cb Cr, w/2 x h/2) (test setup only);
// leaves the texture readable by shaders.
bool UploadNv12(Gpu& gpu, ID3D12Resource* texture, unsigned w, unsigned h, const std::vector<std::uint8_t>& luma, std::vector<ID3D12Resource*>& keep,
                const std::vector<std::uint8_t>* chroma = nullptr) {
    const auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2]{}; UINT rows[2]{}; UINT64 sizes[2]{}, total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 2, 0, layouts, rows, sizes, &total);
    auto* staging = Buffer(gpu.device, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!staging) return false;
    keep.push_back(staging);
    std::uint8_t* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    for (UINT r = 0; r < rows[0]; ++r) std::memcpy(mapped + layouts[0].Offset + r * layouts[0].Footprint.RowPitch, luma.data() + static_cast<std::size_t>(r) * w, w);
    for (UINT r = 0; r < rows[1]; ++r) {
        if (chroma) std::memcpy(mapped + layouts[1].Offset + r * layouts[1].Footprint.RowPitch, chroma->data() + static_cast<std::size_t>(r) * w, w);
        else std::memset(mapped + layouts[1].Offset + r * layouts[1].Footprint.RowPitch, 128, w);
    }
    staging->Unmap(0, nullptr);
    for (unsigned s = 0; s < 2; ++s) {
        D3D12_TEXTURE_COPY_LOCATION to{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; to.SubresourceIndex = s;
        D3D12_TEXTURE_COPY_LOCATION from{staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = layouts[s];
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    const auto barrier = Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.list->ResourceBarrier(1, &barrier);
    (void)h; return true;
}
// Fixtures in source pixels. Hard: point-sampled half-plane (aliased staircase). Soft: 16x16 supersampled coverage.
double EdgeX(double y) { return kCenter + (y - kCenter) * kSlope; }
std::vector<std::uint8_t> EdgeImage(bool antialiased) {
    std::vector<std::uint8_t> image(S * S);
    for (unsigned y = 0; y < S; ++y) for (unsigned x = 0; x < S; ++x) {
        double coverage = 0;
        if (!antialiased) coverage = x + 0.5 > EdgeX(y + 0.5) ? 1 : 0;
        else {
            for (unsigned sy = 0; sy < 16; ++sy) for (unsigned sx = 0; sx < 16; ++sx)
                coverage += x + (sx + 0.5) / 16 > EdgeX(y + (sy + 0.5) / 16) ? 1 : 0;
            coverage /= 256;
        }
        image[y * S + x] = static_cast<std::uint8_t>(std::lround(kDark + (kBright - kDark) * coverage));
    }
    return image;
}
std::vector<std::uint8_t> NoiseImage() {
    std::vector<std::uint8_t> image(S * S);
    std::uint32_t state = 12345;
    for (auto& v : image) { state = state * 1664525u + 1013904223u; v = static_cast<std::uint8_t>(kFlat + static_cast<int>((state >> 24) % 5) - 2); }
    return image;
}
struct Measured { double overshoot, width, jaggies, bias, noise; unsigned rows; };
// Chroma fixtures: a vertical colour edge between source columns kChromaEdge - 1 and kChromaEdge (odd, so the edge
// splits a chroma texel's pair of columns, as it generally does in 4:2:0 video). Colours are Y'CbCr codes.
constexpr unsigned kChromaEdge = 45;
struct Colour { unsigned y, cb, cr; };
struct ChromaFixture { const char* name; Colour a, b; };
constexpr ChromaFixture kChromaFixtures[]{
    {"coincident luma/colour edge", {60, 110, 180}, {170, 150, 100}}, // Dark red | light blue-grey: the common case.
    {"isoluminant colour edge", {120, 100, 170}, {120, 160, 90}},      // Colour changes, luma does not.
    {"flat colour", {120, 100, 170}, {120, 100, 170}}};
// 4:4:4 source subsampled to 4:2:0 by averaging each pair of columns (the fixtures are constant vertically).
void ChromaImage(const ChromaFixture& f, std::vector<std::uint8_t>& luma, std::vector<std::uint8_t>& chroma) {
    luma.resize(S * S); chroma.resize(S * S / 2);
    for (unsigned y = 0; y < S; ++y) for (unsigned x = 0; x < S; ++x) luma[y * S + x] = static_cast<std::uint8_t>(x < kChromaEdge ? f.a.y : f.b.y);
    for (unsigned y = 0; y < S / 2; ++y) for (unsigned x = 0; x < S / 2; ++x) {
        const unsigned left = static_cast<unsigned>(2 * x < kChromaEdge) + static_cast<unsigned>(2 * x + 1 < kChromaEdge); // Of the pair, in f.a.
        chroma[(y * (S / 2) + x) * 2] = static_cast<std::uint8_t>((left * f.a.cb + (2 - left) * f.b.cb + 1) / 2);
        chroma[(y * (S / 2) + x) * 2 + 1] = static_cast<std::uint8_t>((left * f.a.cr + (2 - left) * f.b.cr + 1) / 2);
    }
}
// Chroma (in 8-bit code steps, 224 per unit) of an RGBA8 output pixel, inverting the BT.709 limited-range matrix.
void ChromaOf(const std::uint8_t* rgba, double& cb, double& cr) {
    const double r = rgba[0] / 255.0, g = rgba[1] / 255.0, b = rgba[2] / 255.0, y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    cb = (b - y) / 1.8556 * 224.0 + 128.0; cr = (r - y) / 1.5748 * 224.0 + 128.0;
}
struct ChromaMeasured { double error, width, overshoot; };
}

int main() {
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
    if (!gpu.device) { std::puts("SKIP no hardware D3D12 device"); return 77; }
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = gpu.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&gpu.queue));
    if (SUCCEEDED(hr)) hr = gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    if (SUCCEEDED(hr)) hr = gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    if (SUCCEEDED(hr)) hr = gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator, nullptr, IID_PPV_ARGS(&gpu.list));
    media::render::ColorPipeline color;
    if (SUCCEEDED(hr)) hr = color.Create(gpu.device);
    media::render::NeuralUpscalePass neural;
    if (SUCCEEDED(hr)) hr = neural.Create(gpu.device, gpu.queue, media::render::kNeuralWeights, 1920, 1080);
    // Metrics pass: t0 image, u0 result words, b0 constants.
    ID3D12RootSignature* metrics_root = nullptr; ID3D12PipelineState* metrics = nullptr;
    {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[0].DescriptorTable = {1, &range};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[1].Descriptor = {0, 0};
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[2].Constants = {0, 0, 8};
        const D3D12_ROOT_SIGNATURE_DESC signature{3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *blob = nullptr, *errors = nullptr;
        if (SUCCEEDED(hr)) hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (SUCCEEDED(hr)) hr = gpu.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&metrics_root));
        ReleaseCom(blob); ReleaseCom(errors);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = metrics_root; pso.CS = {g_EdgeMetricsShader, sizeof(g_EdgeMetricsShader)};
        if (SUCCEEDED(hr)) hr = gpu.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&metrics));
    }
    // Descriptors: sources (luma, chroma) x 4 at [0..7] (hard, soft, noise, timing), output UAV/SRV at [8]/[9], timing UAV [10],
    // neural 2x luma UAV/SRV for the fixtures at [11]/[12] and for the timing source at [13]/[14].
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 15, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (SUCCEEDED(hr)) hr = gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    ID3D12Resource* sources[4]{};
    for (unsigned i = 0; i < 4; ++i)
        sources[i] = Texture(gpu.device, i == 3 ? PW : S, i == 3 ? PH : S, DXGI_FORMAT_NV12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto* output = Texture(gpu.device, O, O, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* display = Texture(gpu.device, DW, DH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* doubled = Texture(gpu.device, 2 * S, 2 * S, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* doubled_timing = Texture(gpu.device, 2 * PW, 2 * PH, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12QueryHeap* queries = nullptr;
    D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 64, 0};
    if (SUCCEEDED(hr)) hr = gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries));
    auto* stamps = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, 64 * 8, D3D12_RESOURCE_STATE_COPY_DEST);
    if (FAILED(hr) || !heap || !output || !display || !doubled || !doubled_timing || !stamps || !sources[0] || !sources[1] || !sources[2] || !sources[3]) {
        std::printf("FAIL setup %08lx\n", static_cast<unsigned long>(hr)); return 1;
    }
    const auto stride = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](unsigned i) { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * stride; return h; };
    auto gpu_handle = [&](unsigned i) { auto h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(i) * stride; return h; };
    for (unsigned i = 0; i < 4; ++i) for (unsigned plane = 0; plane < 2; ++plane) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
        gpu.device->CreateShaderResourceView(sources[i], &srv, cpu(i * 2 + plane));
    }
    gpu.device->CreateUnorderedAccessView(output, nullptr, nullptr, cpu(8));
    gpu.device->CreateShaderResourceView(output, nullptr, cpu(9));
    gpu.device->CreateUnorderedAccessView(display, nullptr, nullptr, cpu(10));
    gpu.device->CreateUnorderedAccessView(doubled, nullptr, nullptr, cpu(11)); gpu.device->CreateShaderResourceView(doubled, nullptr, cpu(12));
    gpu.device->CreateUnorderedAccessView(doubled_timing, nullptr, nullptr, cpu(13)); gpu.device->CreateShaderResourceView(doubled_timing, nullptr, cpu(14));
    // Neural luma, as the renderer records it: the pass into the 2x texture, then the conversion reading it (mode 2).
    auto record_neural = [&](unsigned source, unsigned w, unsigned h, ID3D12Resource* target2x, unsigned uav, unsigned out_uav, const ColorConstants& c) {
        auto b = Transition(target2x, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); gpu.list->ResourceBarrier(1, &b);
        neural.Invalidate(); // Other sources and targets in turn: the network alone, no temporal reuse.
        neural.Record(gpu.list, gpu_handle(source * 2), gpu_handle(uav), 0, 0, w, h);
        b = Transition(target2x, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); gpu.list->ResourceBarrier(1, &b);
        color.Record(gpu.list, gpu_handle(source * 2), gpu_handle(out_uav), c, gpu_handle(uav + 1));
        b = Transition(target2x, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON); gpu.list->ResourceBarrier(1, &b);
    };
    std::vector<ID3D12Resource*> staging;
    std::vector<std::uint8_t> timing(PW * PH);
    std::uint32_t state = 7;
    for (auto& v : timing) { state = state * 1664525u + 1013904223u; v = static_cast<std::uint8_t>(16 + (state >> 24) % 220); }
    CHECK(UploadNv12(gpu, sources[0], S, S, EdgeImage(false), staging) && UploadNv12(gpu, sources[1], S, S, EdgeImage(true), staging) &&
          UploadNv12(gpu, sources[2], S, S, NoiseImage(), staging) && UploadNv12(gpu, sources[3], PW, PH, timing, staging));
    CHECK(gpu.Submit());
    for (auto* s : staging) ReleaseCom(s);
    staging.clear();
    ID3D12DescriptorHeap* heaps[]{heap};
    auto constants = [](unsigned sw, unsigned sh, unsigned dw, unsigned dh, std::uint32_t mode, float sharpness, float noise) {
        return ColorConstants{dw, dh, sw, sh, 0, 0, sw, sh, 1, 0, 0, 1, 1, 0, 1000.0f, 203.0f, mode, sharpness, noise, 0};
    };
    // Converts source `index` to the 288 x 288 output (or `size` for 1:1) and measures it.
    auto measure = [&](unsigned index, std::uint32_t mode, float sharpness, float noise, double level, unsigned size = O) {
        gpu.list->SetDescriptorHeaps(1, heaps);
        if (mode == media::render::kNeural) record_neural(index, S, S, doubled, 11, 8, constants(S, S, size, size, mode, sharpness, noise));
        else color.Record(gpu.list, gpu_handle(index * 2), gpu_handle(8), constants(S, S, size, size, mode, sharpness, noise));
        auto barrier = Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu.list->ResourceBarrier(1, &barrier);
        auto* result = Buffer(gpu.device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const float scale = static_cast<float>(S) / static_cast<float>(size);
        const float dark = static_cast<float>(index == 2 ? level : Encoded(kDark)), bright = static_cast<float>(Encoded(kBright));
        struct { float dark, bright, scale, cx, cy, slope; std::uint32_t margin, unused; } m{dark, bright, scale, static_cast<float>(kCenter),
            static_cast<float>(kCenter), static_cast<float>(kSlope), 3 * size / S, 0};
        gpu.list->SetComputeRootSignature(metrics_root); gpu.list->SetPipelineState(metrics);
        gpu.list->SetComputeRootDescriptorTable(0, gpu_handle(9)); gpu.list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
        gpu.list->SetComputeRoot32BitConstants(2, 8, &m, 0);
        gpu.list->Dispatch((size + 63) / 64, 1, 1);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        gpu.list->ResourceBarrier(1, &barrier);
        auto* readback = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.list->CopyBufferRegion(readback, 0, result, 0, 24);
        CHECK(gpu.Submit());
        std::uint32_t w[6]{}; void* mapped = nullptr; const D3D12_RANGE range{0, 24};
        if (SUCCEEDED(readback->Map(0, &range, &mapped))) { std::memcpy(w, mapped, 24); const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none); }
        ReleaseCom(readback); ReleaseCom(result);
        const unsigned rows = size - 2 * (3 * size / S);
        const double mean = w[8 / 4] ? static_cast<std::int32_t>(w[3]) / 1000.0 / w[2] : 0;
        const double square = w[2] ? w[4] / 1000.0 / w[2] : 0;
        return Measured{w[0] / 1000.0, static_cast<double>(w[1]) / rows / Up, std::sqrt((std::max)(square - mean * mean, 0.0)) / Up, mean / Up,
                        w[5] / 1000.0 / (static_cast<double>(rows) * size), w[2]};
    };
    // ---- Edge quality at 3x (widths, staircase and bias in source pixels; overshoot and noise in 8-bit steps) ----
    const auto hard_bilinear = measure(0, 0, 0, 0.02f, 0), hard_edge = measure(0, 1, 0, 0.02f, 0), hard_sharp = measure(0, 1, 0.5f, 0.02f, 0);
    const auto soft_bilinear = measure(1, 0, 0, 0.02f, 0), soft_edge = measure(1, 1, 0, 0.02f, 0), soft_sharp = measure(1, 1, 1.0f, 0.02f, 0);
    const auto hard_neural = measure(0, 2, 0, 0.02f, 0), soft_neural = measure(1, 2, 0, 0.02f, 0);
    const auto print = [](const char* name, const Measured& m) {
        std::printf("  %-34s staircase RMS %.3f px, 10-90%% width %.3f px, overshoot %.3f steps, rows %u\n", name, m.jaggies, m.width, m.overshoot, m.rows);
    };
    std::puts("Aliased (point-sampled) edge, 3x upscale:");
    print("bilinear", hard_bilinear); print("edge-directed", hard_edge); print("edge-directed + sharpen 0.5", hard_sharp); print("neural (2x net, then 1.5x)", hard_neural);
    std::puts("Antialiased edge, 3x upscale:");
    print("bilinear", soft_bilinear); print("edge-directed", soft_edge); print("edge-directed + sharpen 1.0", soft_sharp); print("neural (2x net, then 1.5x)", soft_neural);
    // Every row finds exactly one crossing near the true line.
    for (const auto* m : {&hard_bilinear, &hard_edge, &hard_sharp, &soft_bilinear, &soft_edge, &soft_sharp, &hard_neural, &soft_neural})
        CHECK(m->rows == O - 2 * 9 && std::abs(m->bias) < 0.25);
    // Jaggies: the staircase of an aliased edge is suppressed well beyond bilinear filtering.
    CHECK(hard_edge.jaggies < 0.7 * hard_bilinear.jaggies && soft_edge.jaggies < 0.5 * soft_bilinear.jaggies);
    // Sharpness: the reconstructed edge is narrower than bilinear, and sharpening narrows it further.
    CHECK(soft_edge.width < soft_bilinear.width && soft_sharp.width < soft_edge.width && hard_edge.width < hard_bilinear.width);
    // Ringing: no overshoot beyond the two levels (a quarter step of float noise allowed) with and without sharpening,
    // nor from the neural reconstruction (its output is clamped to the samples each sub-pixel interpolates).
    for (const auto* m : {&hard_edge, &hard_sharp, &soft_edge, &soft_sharp, &hard_neural, &soft_neural}) CHECK(m->overshoot <= 0.25);
    // Neural: sharper than bilinear on both edges, and less staircase on the antialiased (camera-like) edge. On the
    // point-sampled edge it does not de-alias (such input is outside its training distribution): edge-directed does.
    CHECK(soft_neural.width < soft_bilinear.width && hard_neural.width < hard_bilinear.width && soft_neural.jaggies < soft_bilinear.jaggies);
    // ---- Zoom (docs/picture.md, "Zoom"): the region cropped before reconstruction, against enlarging after it ----
    // A 4x zoom into the middle 24 x 24 of the 96 x 96 edge, shown 288 x 288 (12 output pixels a sample): converted from the
    // region's own samples, as the renderer zooms (its crop is the region), against the 3x conversion's middle 72 x 72
    // enlarged 4x bilinearly (a picture magnified after its reconstruction). Both are read back and measured alike on the
    // CPU: each row's 50 % crossing against the true line (bias: the region's geometry; spread: the staircase) and the
    // 10-90 % transition width, in output pixels.
    {
        constexpr unsigned R0 = 36, R1 = 60; // The region, in source samples.
        const auto out_desc = output->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT print_layout{}; UINT64 print_bytes = 0;
        gpu.device->GetCopyableFootprints(&out_desc, 0, 1, 0, &print_layout, nullptr, nullptr, &print_bytes);
        auto* zoom_readback = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, print_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        // Converts source `index` (mode, crop) to the 288 x 288 output and reads it back as RGBA8.
        auto convert = [&](unsigned index, std::uint32_t mode, unsigned l, unsigned t, unsigned r, unsigned b, std::vector<std::uint8_t>& pixels_out) {
            gpu.list->SetDescriptorHeaps(1, heaps);
            auto k = constants(S, S, O, O, mode, 0, 0.02f); k.left = l; k.top = t; k.right = r; k.bottom = b;
            if (mode == media::render::kNeural) {
                auto n = Transition(doubled, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); gpu.list->ResourceBarrier(1, &n);
                neural.Invalidate();
                neural.Record(gpu.list, gpu_handle(index * 2), gpu_handle(11), l, t, r, b); // The region alone, as the renderer records a zoom.
                n = Transition(doubled, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); gpu.list->ResourceBarrier(1, &n);
                color.Record(gpu.list, gpu_handle(index * 2), gpu_handle(8), k, gpu_handle(12));
                n = Transition(doubled, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON); gpu.list->ResourceBarrier(1, &n);
            } else color.Record(gpu.list, gpu_handle(index * 2), gpu_handle(8), k);
            auto barrier = Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION to{zoom_readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = print_layout;
            D3D12_TEXTURE_COPY_LOCATION from{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            gpu.list->ResourceBarrier(1, &barrier);
            CHECK(gpu.Submit());
            pixels_out.assign(static_cast<std::size_t>(O) * O * 4, 0);
            std::uint8_t* mapped = nullptr; const D3D12_RANGE range{0, static_cast<SIZE_T>(print_bytes)};
            if (SUCCEEDED(zoom_readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
                for (unsigned y = 0; y < O; ++y)
                    std::memcpy(pixels_out.data() + static_cast<std::size_t>(y) * O * 4, mapped + print_layout.Offset + static_cast<std::size_t>(y) * print_layout.Footprint.RowPitch, O * 4);
                const D3D12_RANGE none{0, 0}; zoom_readback->Unmap(0, &none);
            }
        };
        // The 3x picture's middle (the region, 72 x 72 of it) enlarged 4x, bilinearly between pixel centres.
        auto enlarge = [&](const std::vector<std::uint8_t>& whole, std::vector<std::uint8_t>& out) {
            out.assign(static_cast<std::size_t>(O) * O * 4, 0);
            constexpr double origin = R0 * Up; // 108: the region's corner in the 3x picture.
            for (unsigned y = 0; y < O; ++y) for (unsigned x = 0; x < O; ++x) {
                const double fx = origin + (x + 0.5) / 4.0 - 0.5, fy = origin + (y + 0.5) / 4.0 - 0.5;
                const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy)); const double ax = fx - x0, ay = fy - y0;
                for (unsigned c = 0; c < 4; ++c) {
                    const auto at = [&](int xx, int yy) { xx = (std::clamp)(xx, 0, static_cast<int>(O) - 1); yy = (std::clamp)(yy, 0, static_cast<int>(O) - 1);
                                                          return static_cast<double>(whole[(static_cast<std::size_t>(yy) * O + xx) * 4 + c]); };
                    const double v = (1 - ay) * ((1 - ax) * at(x0, y0) + ax * at(x0 + 1, y0)) + ay * ((1 - ax) * at(x0, y0 + 1) + ax * at(x0 + 1, y0 + 1));
                    out[(static_cast<std::size_t>(y) * O + x) * 4 + c] = static_cast<std::uint8_t>(std::lround(v));
                }
            }
        };
        struct Edge { double bias = 0, staircase = 0, width = 0; unsigned rows = 0; };
        // Output pixel (x, y) shows source sample R0 + (x + 0.5) / 12 (both pictures map so); the true edge passes the
        // source centre at kSlope.
        auto edge = [&](const std::vector<std::uint8_t>& image) {
            const double dark = Encoded(kDark) * 255, bright = Encoded(kBright) * 255, scale = static_cast<double>(O) / (R1 - R0);
            double sum = 0, square = 0, band = 0; unsigned rows = 0;
            for (unsigned y = 24; y + 24 < O; ++y) {
                double previous = 0; bool found = false; double crossing = 0;
                for (unsigned x = 0; x < O; ++x) {
                    const double t = (image[(static_cast<std::size_t>(y) * O + x) * 4 + 1] - dark) / (bright - dark);
                    if (t > 0.1 && t < 0.9) band += 1;
                    if (!found && x && previous < 0.5 && t >= 0.5) { crossing = (x - 1) + (0.5 - previous) / (t - previous); found = true; }
                    previous = t;
                }
                if (!found) continue;
                const double sy = R0 + (y + 0.5) / scale, sx = kCenter + kSlope * (sy - kCenter);
                const double truth = (sx - R0) * scale - 0.5, d = crossing - truth;
                sum += d; square += d * d; ++rows;
            }
            Edge e; e.rows = rows;
            if (rows) { e.bias = sum / rows; e.staircase = std::sqrt((std::max)(square / rows - e.bias * e.bias, 0.0)); e.width = band / (O - 48); }
            return e;
        };
        std::vector<std::uint8_t> zoomed, whole, enlarged;
        const struct { unsigned index; std::uint32_t mode; const char* name; } cases[]{
            {0, 1, "aliased edge, edge-directed"}, {1, 1, "antialiased edge, edge-directed"}, {1, media::render::kNeural, "antialiased edge, neural"}};
        for (const auto& c : cases) {
            convert(c.index, c.mode, R0, R0, R1, R1, zoomed);
            convert(c.index, c.mode, 0, 0, S, S, whole); // The whole picture at 3x, the same way.
            enlarge(whole, enlarged);
            const auto z = edge(zoomed), e = edge(enlarged);
            std::printf("Zoom 4x, %s: region reconstructed: bias %+.2f px, staircase %.2f px, width %.1f px; enlarged after: bias %+.2f px, staircase %.2f px, width %.1f px\n",
                        c.name, z.bias, z.staircase, z.width, e.bias, e.staircase, e.width);
            // The region lands where the source's edge is (the zoom's geometry is exact to a quarter of a sample), and no row
            // loses its crossing.
            CHECK(z.rows == O - 48 && e.rows == O - 48 && std::abs(z.bias) < 3.0 && std::abs(e.bias) < 3.0);
            // Reconstructed at the zoomed scale: a narrower edge (measured 7-17 % narrower), placed at least as exactly, with
            // no more staircase (the source's own sampling: both keep it, within a few hundredths of a pixel).
            CHECK(z.width < 0.95 * e.width && std::abs(z.bias) <= std::abs(e.bias) + 0.05 && z.staircase < 1.4 * e.staircase);
        }
        ReleaseCom(zoom_readback);
    }
    // ---- Noise: a flat field with +-2 code noise is not amplified by sharpening (the noise floor gates it) ----
    const double flat = Encoded(kFlat);
    const auto quiet = measure(2, 1, 0, 0.02f, flat), sharpened = measure(2, 1, 1.0f, 0.02f, flat), ungated = measure(2, 1, 1.0f, 0.0f, flat);
    std::printf("Flat field, +-2 code noise: mean |deviation| %.3f steps unsharpened, %.3f sharpened (noise floor 0.02), %.3f without the floor\n",
                quiet.noise, sharpened.noise, ungated.noise);
    CHECK(sharpened.noise <= quiet.noise * 1.02 && ungated.noise > quiet.noise * 1.05);
    // ---- 1:1: edge-directed mode falls back to exact bilinear (texel centres), so the output is unchanged ----
    const auto same_bilinear = measure(1, 0, 0, 0.02f, 0, S), same_edge = measure(1, 1, 0, 0.02f, 0, S);
    CHECK(same_bilinear.width == same_edge.width && same_bilinear.jaggies == same_edge.jaggies && same_bilinear.rows == same_edge.rows);
    // ---- Guided chroma: colour edges follow luma edges; isoluminant and flat colour are left exactly as bilinear ----
    ID3D12DescriptorHeap* chroma_heap = nullptr; // Fixture planes at [0..5], output UAV at [6].
    D3D12_DESCRIPTOR_HEAP_DESC chroma_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 7, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    CHECK(SUCCEEDED(gpu.device->CreateDescriptorHeap(&chroma_desc, IID_PPV_ARGS(&chroma_heap))));
    ID3D12Resource* chroma_sources[3]{};
    for (auto*& t : chroma_sources) t = Texture(gpu.device, S, S, DXGI_FORMAT_NV12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto* chroma_output = Texture(gpu.device, O, O, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto output_desc = chroma_output->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 footprint_bytes = 0;
    gpu.device->GetCopyableFootprints(&output_desc, 0, 1, 0, &footprint, nullptr, nullptr, &footprint_bytes);
    auto* chroma_readback = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, footprint_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    auto chroma_cpu = [&](unsigned i) { auto h = chroma_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * stride; return h; };
    auto chroma_gpu = [&](unsigned i) { auto h = chroma_heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(i) * stride; return h; };
    for (unsigned i = 0; i < 3; ++i) {
        std::vector<std::uint8_t> luma, chroma; ChromaImage(kChromaFixtures[i], luma, chroma);
        CHECK(UploadNv12(gpu, chroma_sources[i], S, S, luma, staging, &chroma));
        for (unsigned plane = 0; plane < 2; ++plane) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
            gpu.device->CreateShaderResourceView(chroma_sources[i], &srv, chroma_cpu(i * 2 + plane));
        }
    }
    gpu.device->CreateUnorderedAccessView(chroma_output, nullptr, nullptr, chroma_cpu(6));
    CHECK(gpu.Submit());
    for (auto* t : staging) ReleaseCom(t);
    staging.clear();
    std::vector<std::uint8_t> pixels, reference;
    // Converts fixture `index` to `size` x `size` with luma `mode` and the chroma mode; returns the RGBA8 pixels.
    auto convert_chroma = [&](unsigned index, unsigned size, std::uint32_t mode, std::uint32_t chroma_mode, std::vector<std::uint8_t>& out) {
        ID3D12DescriptorHeap* chroma_heaps[]{chroma_heap};
        gpu.list->SetDescriptorHeaps(1, chroma_heaps);
        auto k = constants(S, S, size, size, mode, 0, 0.02f); k.chroma_mode = chroma_mode;
        color.Record(gpu.list, chroma_gpu(index * 2), chroma_gpu(6), k);
        auto barrier = Transition(chroma_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION to{chroma_readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{chroma_output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        gpu.list->ResourceBarrier(1, &barrier);
        CHECK(gpu.Submit());
        out.assign(static_cast<std::size_t>(size) * size * 4, 0);
        std::uint8_t* mapped = nullptr; const D3D12_RANGE range{0, static_cast<SIZE_T>(footprint_bytes)};
        if (SUCCEEDED(chroma_readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
            for (unsigned y = 0; y < size; ++y)
                std::memcpy(out.data() + static_cast<std::size_t>(y) * size * 4, mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch, size * 4);
            const D3D12_RANGE none{0, 0}; chroma_readback->Unmap(0, &none);
        }
    };
    // Against the true 4:4:4 edge, over the middle rows: mean chroma error, bleeding width (pixels more than 10 % of
    // the colour step away from the true colour, per row, in source pixels) and overshoot beyond the two colours.
    auto chroma_metrics = [&](const ChromaFixture& f, const std::vector<std::uint8_t>& out, unsigned size) {
        const double step = std::hypot(double(f.b.cb) - f.a.cb, double(f.b.cr) - f.a.cr);
        double error = 0, wide = 0, overshoot = 0; unsigned count = 0, rows = 0;
        for (unsigned y = size / 4; y < 3 * size / 4; ++y, ++rows) for (unsigned x = 0; x < size; ++x) {
            double cb = 0, cr = 0; ChromaOf(out.data() + (static_cast<std::size_t>(y) * size + x) * 4, cb, cr);
            const bool left = (x + 0.5) * S / size < kChromaEdge;
            const double tb = left ? f.a.cb : f.b.cb, tr = left ? f.a.cr : f.b.cr, d = std::hypot(cb - tb, cr - tr);
            error += d; ++count;
            if (step > 0 && d > 0.1 * step) wide += 1.0;
            overshoot = (std::max)({overshoot, (std::min)(f.a.cb, f.b.cb) - cb, cb - (std::max)(f.a.cb, f.b.cb), (std::min)(f.a.cr, f.b.cr) - cr, cr - (std::max)(f.a.cr, f.b.cr)});
        }
        return ChromaMeasured{error / count, wide / rows * S / size, overshoot};
    };
    auto max_difference = [](const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
        int worst = 0; for (std::size_t i = 0; i < a.size(); ++i) worst = (std::max)(worst, std::abs(int(a[i]) - int(b[i]))); return worst;
    };
    std::puts("Chroma, 4:2:0 edge between source columns 44 and 45 (width in source pixels, error and overshoot in code steps):");
    for (unsigned size : {S, O}) {
        const std::uint32_t mode = size == S ? media::render::kBilinear : media::render::kEdgeDirected;
        ChromaMeasured m[3][2]{};
        for (unsigned i = 0; i < 3; ++i) {
            convert_chroma(i, size, mode, media::render::kBilinearChroma, reference); m[i][0] = chroma_metrics(kChromaFixtures[i], reference, size);
            convert_chroma(i, size, mode, media::render::kGuidedChroma, pixels); m[i][1] = chroma_metrics(kChromaFixtures[i], pixels, size);
            const int changed = max_difference(reference, pixels);
            std::printf("  %s %-28s bilinear: error %.2f, width %.2f px | guided: error %.2f, width %.2f px, overshoot %.2f; max RGB change %d\n",
                        size == S ? "1:1" : "3x ", kChromaFixtures[i].name, m[i][0].error, m[i][0].width, m[i][1].error, m[i][1].width, m[i][1].overshoot, changed);
            if (i > 0) CHECK(changed <= 1); // Isoluminant and flat: no luma variation, so exactly bilinear (8-bit rounding).
            CHECK(m[i][1].overshoot <= 1.5);  // Never beyond the two colours (RGB quantisation: about a code step).
        }
        // Coincident edge: bleeding at least halved, and lower error overall.
        CHECK(m[0][1].width <= 0.5 * m[0][0].width && m[0][1].error < 0.6 * m[0][0].error);
    }
    // ---- Split comparison (kCompare): left of the seam exactly the plain conversion (bilinear luma and chroma, no
    // sharpening), right of it exactly the enhanced one, and the two seam columns as defined (Reconstruct.hlsli) ----
    auto read_rgba = [&](ID3D12Resource* texture, unsigned size, std::vector<std::uint8_t>& out) {
        auto barrier = Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION to{chroma_readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        gpu.list->ResourceBarrier(1, &barrier);
        CHECK(gpu.Submit());
        out.assign(static_cast<std::size_t>(size) * size * 4, 0);
        std::uint8_t* mapped = nullptr; const D3D12_RANGE range{0, static_cast<SIZE_T>(footprint_bytes)};
        if (SUCCEEDED(chroma_readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
            for (unsigned y = 0; y < size; ++y)
                std::memcpy(out.data() + static_cast<std::size_t>(y) * size * 4, mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch, size * 4);
            const D3D12_RANGE none{0, 0}; chroma_readback->Unmap(0, &none);
        }
    };
    // The hard slanted edge (it crosses the seam), into the output at [8]; `neural`: through the network first.
    auto convert_split = [&](bool neural_case, const ColorConstants& k, std::vector<std::uint8_t>& out) {
        gpu.list->SetDescriptorHeaps(1, heaps);
        if (neural_case) record_neural(0, S, S, doubled, 11, 8, k);
        else color.Record(gpu.list, gpu_handle(0), gpu_handle(8), k);
        read_rgba(output, O, out);
    };
    std::puts("Split comparison (Original | Enhanced), 3x:");
    struct SplitCase { const char* name; std::uint32_t mode; float sharpness; bool neural; };
    for (const SplitCase& c : {SplitCase{"edge-directed + guided chroma + sharpening 0.5", media::render::kEdgeDirected, 0.5f, false},
                               SplitCase{"neural + guided chroma", media::render::kNeural, 0.0f, true}}) {
        auto enhanced_k = constants(S, S, O, O, c.mode, c.sharpness, 0.02f);
        enhanced_k.chroma_mode = media::render::kGuidedChroma; enhanced_k.filter_flags = media::render::kAreaPrefilter | media::render::kDither;
        auto split_k = enhanced_k; split_k.filter_flags |= media::render::kCompare;
        auto plain_k = constants(S, S, O, O, media::render::kBilinear, 0, 0.02f); plain_k.filter_flags = media::render::kDither;
        std::vector<std::uint8_t> split, plain, enhanced;
        convert_split(c.neural, split_k, split); convert_split(c.neural, enhanced_k, enhanced); convert_split(c.neural, plain_k, plain);
        const unsigned seam = O / 2; unsigned left_wrong = 0, right_wrong = 0, seam_wrong = 0, differs = 0;
        for (unsigned y = 0; y < O; ++y) for (unsigned x = 0; x < O; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * O + x) * 4;
            for (unsigned ch = 0; ch < 3; ++ch) {
                const int s = split[at + ch], p = plain[at + ch], e = enhanced[at + ch];
                if (x + 1 < seam) left_wrong += s != p;
                else if (x > seam) { right_wrong += s != e; differs += p != e; }
                else if (x + 1 == seam) seam_wrong += std::abs(s - 0.25 * p) > 1.0;          // Dark side (from 8-bit values: +-1).
                else seam_wrong += std::abs(s - (e + 0.75 * (255 - e))) > 1.0;               // Light side.
            }
        }
        std::printf("  %-48s left vs plain: %u wrong; right vs enhanced: %u wrong (enhanced differs from plain in %u values); seam: %u wrong\n",
                    c.name, left_wrong, right_wrong, differs, seam_wrong);
        CHECK(left_wrong == 0 && right_wrong == 0 && seam_wrong == 0 && differs > 0);
    }
    { // Recording the comparison allocates nothing.
        auto k = constants(S, S, O, O, media::render::kEdgeDirected, 0.5f, 0.02f); k.chroma_mode = media::render::kGuidedChroma; k.filter_flags = media::render::kCompare;
        ID3D12DescriptorHeap* chroma_heaps[]{chroma_heap};
        allocation_probe::Begin();
        for (unsigned i = 0; i < 10; ++i) { gpu.list->SetDescriptorHeaps(1, chroma_heaps); color.Record(gpu.list, chroma_gpu(0), chroma_gpu(6), k); if (!gpu.Submit()) break; }
        const auto allocations = allocation_probe::End();
        std::printf("  recording and running the comparison: %llu allocations\n", static_cast<unsigned long long>(allocations));
        CHECK(allocations == 0);
    }
    ReleaseCom(chroma_readback); ReleaseCom(chroma_output); for (auto*& t : chroma_sources) ReleaseCom(t); ReleaseCom(chroma_heap);
    // ---- Cost: 1280x720 NV12 -> 1920x1080, GPU timestamps around the conversion dispatch alone ----
    UINT64 frequency = 0; CHECK(SUCCEEDED(gpu.queue->GetTimestampFrequency(&frequency)));
    struct Config { const char* name; std::uint32_t mode; float sharpness; unsigned w, h; std::uint32_t chroma = 0, flags = 0; } configs[]{
        {"bilinear (point loads)", 0, 0, DW, DH}, {"edge-directed", 1, 0, DW, DH}, {"edge-directed + sharpening", 1, 0.5f, DW, DH},
        {"1:1 720p", 0, 0, PW, PH}, {"1:1 720p + sharpening", 1, 0.5f, PW, PH}, {"neural pass + conversion", 2, 0, DW, DH},
        {"bilinear + guided chroma", 0, 0, DW, DH, 1}, {"edge-directed + guided chroma", 1, 0, DW, DH, 1}, {"1:1 720p + guided chroma", 0, 0, PW, PH, 1},
        {"comparison: edge + guided", 1, 0, DW, DH, 1, media::render::kCompare}};
    constexpr unsigned kRuns = 12, kConfigs = sizeof(configs) / sizeof(configs[0]);
    double best[kConfigs]{};
    auto convert = [&](const Config& c) {
        auto k = constants(PW, PH, c.w, c.h, c.mode, c.sharpness, 0.02f); k.chroma_mode = c.chroma; k.filter_flags = c.flags;
        if (c.mode == media::render::kNeural) record_neural(3, PW, PH, doubled_timing, 13, 10, k);
        else color.Record(gpu.list, gpu_handle(6), gpu_handle(10), k);
    };
    allocation_probe::Begin();
    for (unsigned c = 0; c < kConfigs; ++c) {
        gpu.list->SetDescriptorHeaps(1, heaps);
        // Warm-up burst: an integrated GPU idles at a low clock and ramps only under sustained load.
        for (unsigned r = 0; r < (configs[c].mode == media::render::kNeural ? 12u : 120u); ++r) convert(configs[c]);
        for (unsigned r = 0; r < kRuns; ++r) {
            gpu.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, r * 2);
            convert(configs[c]);
            gpu.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, r * 2 + 1);
            D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; barrier.UAV.pResource = display;
            gpu.list->ResourceBarrier(1, &barrier); // Serializes runs so each interval is one dispatch.
        }
        gpu.list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, kRuns * 2, stamps, 0);
        CHECK(gpu.Submit());
        std::uint64_t ticks[kRuns * 2]{}; void* mapped = nullptr; const D3D12_RANGE range{0, sizeof(ticks)};
        if (SUCCEEDED(stamps->Map(0, &range, &mapped))) { std::memcpy(ticks, mapped, sizeof(ticks)); const D3D12_RANGE none{0, 0}; stamps->Unmap(0, &none); }
        double times[kRuns]{};
        for (unsigned r = 0; r < kRuns; ++r) times[r] = static_cast<double>(ticks[r * 2 + 1] - ticks[r * 2]) * 1000.0 / static_cast<double>(frequency);
        std::sort(times, times + kRuns);
        best[c] = times[0]; // Guards use the fastest run: interference (compositor, other GPU work) only adds time.
        std::printf("  %-28s %.3f ms median, %.3f ms best (%ux%u -> %ux%u)\n", configs[c].name, times[kRuns / 2], times[0], PW, PH, configs[c].w, configs[c].h);
    }
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0);
    // Regression guards within one run (GPU clocks differ between machines and power states): the point-load
    // bilinear base must stay well below the previous sampler-based pass (4.6 ms here at 1080p), and the
    // reconstruction and sharpening stages within their measured multiples of it.
    CHECK(best[0] < 4.0 && best[1] < 3.0 * best[0] && best[2] < 5.0 * best[0] && best[4] < 3.0 * best[3]);
    CHECK(best[6] < 2.5 * best[0] && best[7] < 1.8 * best[1] && best[8] < 2.5 * best[3]); // Guided chroma: bounded extra cost.
    CHECK(best[9] < 1.2 * best[7]); // The comparison costs no more than its enhanced half (the plain half is cheaper).
    ReleaseCom(stamps); ReleaseCom(queries); ReleaseCom(display); ReleaseCom(output); ReleaseCom(doubled); ReleaseCom(doubled_timing); neural.Release();
    for (auto*& s : sources) ReleaseCom(s);
    ReleaseCom(heap); ReleaseCom(metrics); ReleaseCom(metrics_root); color.Release();
    ReleaseCom(gpu.list); ReleaseCom(gpu.allocator); ReleaseCom(gpu.fence); ReleaseCom(gpu.queue); ReleaseCom(gpu.device);
    std::printf("Upscaling and sharpening: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
