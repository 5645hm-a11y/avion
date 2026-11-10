// Picture controls and micro-motion on the GPU (docs/picture.md, "Picture controls and micro-motion"), through the production
// colour pipeline (ColorPipeline, Reconstruct.hlsli) on synthetic NV12 pictures converted 1:1 and read back:
// - neutral controls with the grade flag set are bit-identical to no controls;
// - shadow lift: every column of a luma ramp below Y' 0.5 rises and none up to 0.75 falls (black stays black; just below 0.75
//   the change is under half a code), every column from 0.75 up is bit-identical,
//   nothing clips that did not; the ramp stays monotonic; Y' 0.10 rises to about 0.19 at lift 0.6;
// - saturation 0 makes a coloured picture grey (R = G = B everywhere);
// - micro-motion: a 16 x 16 patch pulsing +-2 codes around a flat grey comes out pulsing 4 to 8 times as much (visible), the
//   flat surround bit-identical to the conversion without it; a bright bar moving 8 pixels a picture converts bit-identically
//   with and without micro-motion (no ghost, no trail);
// - the statistics pass's new fields: the brightest sample, and moving blocks 0 for a repeated picture and all of them for a
//   picture that changed everywhere.
#include "eq_picture_gpu.hpp"
#include "media/render/ColorPipeline.hpp"
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
template <class T> void ReleaseCom(T*& p) { if (p) { p->Release(); p = nullptr; } }
using media::render::ColorConstants;
constexpr unsigned N = 64; // 64 x 64 pictures, converted 1:1.
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
    ~Gpu() { ReleaseCom(list); ReleaseCom(allocator); ReleaseCom(fence); ReleaseCom(queue); ReleaseCom(device); }
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
ID3D12Resource* Texture(ID3D12Device* device, DXGI_FORMAT format, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = N; desc.Height = N;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = flags;
    ID3D12Resource* resource = nullptr;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource))) ? resource : nullptr;
}
struct Picture { std::uint8_t luma[N * N]; std::uint8_t chroma[N * N / 2]; }; // NV12: Y, then interleaved Cb Cr at half size.
using Rgba = std::vector<std::uint8_t>; // N * N * 4.

struct Harness {
    Gpu gpu;
    media::render::ColorPipeline color;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource *source = nullptr, *output = nullptr, *upload = nullptr, *readback = nullptr, *stats = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT planes[2]{}, image{};
    UINT stride = 0;
    bool Create() {
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
        return gpu.device != nullptr;
    }
    bool Prepare() {
        D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = gpu.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&gpu.queue));
        if (SUCCEEDED(hr)) hr = gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
        if (SUCCEEDED(hr)) hr = gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
        if (SUCCEEDED(hr)) hr = gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator, nullptr, IID_PPV_ARGS(&gpu.list));
        if (SUCCEEDED(hr)) hr = color.Create(gpu.device);
        if (SUCCEEDED(hr)) hr = color.PrepareMotion(gpu.device, N * N);
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (SUCCEEDED(hr)) hr = gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
        if (FAILED(hr)) { std::printf("FAIL picture setup %08lx\n", static_cast<unsigned long>(hr)); return false; }
        source = Texture(gpu.device, DXGI_FORMAT_NV12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
        output = Texture(gpu.device, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!source || !output) return false;
        const auto desc = source->GetDesc(); UINT rows[2]{}; UINT64 sizes[2]{}, total = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 2, 0, planes, rows, sizes, &total);
        upload = Buffer(gpu.device, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        const auto out = output->GetDesc(); UINT64 bytes = 0;
        gpu.device->GetCopyableFootprints(&out, 0, 1, 0, &image, nullptr, nullptr, &bytes);
        readback = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        stats = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!upload || !readback || !stats) return false;
        stride = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (unsigned plane = 0; plane < 2; ++plane) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
            gpu.device->CreateShaderResourceView(source, &srv, Cpu(plane));
        }
        gpu.device->CreateUnorderedAccessView(output, nullptr, nullptr, Cpu(2));
        return true;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned i) const { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * stride; return h; }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu_(unsigned i) const { auto h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(i) * stride; return h; }
    // Uploads `p`, converts it with `c` (and the statistics pass when `with_stats`), reads the RGBA image back.
    bool Convert(const Picture& p, const ColorConstants& c, Rgba& rgba, std::uint32_t* stats_words = nullptr) {
        std::uint8_t* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        for (unsigned r = 0; r < N; ++r) std::memcpy(mapped + planes[0].Offset + r * planes[0].Footprint.RowPitch, p.luma + r * N, N);
        for (unsigned r = 0; r < N / 2; ++r) std::memcpy(mapped + planes[1].Offset + r * planes[1].Footprint.RowPitch, p.chroma + r * N, N);
        upload->Unmap(0, nullptr);
        auto b = Transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST); gpu.list->ResourceBarrier(1, &b);
        for (unsigned s = 0; s < 2; ++s) {
            D3D12_TEXTURE_COPY_LOCATION to{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; to.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION from{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = planes[s];
            gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        D3D12_RESOURCE_BARRIER two[]{Transition(source, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                     Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        gpu.list->ResourceBarrier(2, two);
        ID3D12DescriptorHeap* heaps[]{heap}; gpu.list->SetDescriptorHeaps(1, heaps);
        color.Record(gpu.list, Gpu_(0), Gpu_(2), c);
        if (stats_words) color.RecordStats(gpu.list, c, stats, 0);
        two[0] = Transition(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        two[1] = Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->ResourceBarrier(2, two);
        D3D12_TEXTURE_COPY_LOCATION to{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = image;
        D3D12_TEXTURE_COPY_LOCATION from{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        b = Transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON); gpu.list->ResourceBarrier(1, &b);
        if (!gpu.Submit()) return false;
        rgba.assign(N * N * 4, 0);
        if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        for (unsigned r = 0; r < N; ++r) std::memcpy(rgba.data() + r * N * 4, mapped + image.Offset + r * image.Footprint.RowPitch, N * 4);
        const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
        if (stats_words) {
            if (FAILED(stats->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
            std::memcpy(stats_words, mapped, media::render::ColorPipeline::kStatsBytes); stats->Unmap(0, &none);
        }
        return true;
    }
    ~Harness() { ReleaseCom(stats); ReleaseCom(readback); ReleaseCom(upload); ReleaseCom(output); ReleaseCom(source); ReleaseCom(heap); color.Release(); }
};
ColorConstants Plain() { // BT.709 limited range, 8-bit, SDR, 1:1, no reconstruction, no dither (an exact reference).
    ColorConstants c{N, N, N, N, 0, 0, N, N, 1, 0, 0, 1, 1, 0, 1000.0f, 203.0f, 0, 0.0f, 0.02f, 0, 0.0f, 0, 0};
    c.contrast = 1; c.saturation = 1; c.gamma = 1; c.micro_gain = 1;
    return c;
}
double Luma(const Rgba& p, unsigned x, unsigned y) {
    const auto* q = p.data() + (y * N + x) * 4;
    return (0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2]) / 255.0;
}
}

namespace eq_picture_gpu {
int Run(int& failures) {
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (false)
    Harness h;
    if (!h.Create()) { std::puts("Picture: no hardware D3D12 device, skipped"); return -1; }
    if (!h.Prepare()) { ++failures; return 0; }
    static Picture ramp, colour;
    for (unsigned y = 0; y < N; ++y) for (unsigned x = 0; x < N; ++x) ramp.luma[y * N + x] = static_cast<std::uint8_t>(16 + x * 219 / (N - 1));
    std::memset(ramp.chroma, 128, sizeof(ramp.chroma));
    std::memcpy(colour.luma, ramp.luma, sizeof(colour.luma));
    for (unsigned i = 0; i < N * N / 2; i += 2) { colour.chroma[i] = 90; colour.chroma[i + 1] = 180; }
    Rgba plain, graded;
    // Neutral controls (the grade flag set, every value neutral): bit-identical.
    CHECK(h.Convert(ramp, Plain(), plain));
    auto c = Plain(); c.picture_flags = media::render::kGrade;
    CHECK(h.Convert(ramp, c, graded));
    CHECK(graded == plain);
    // Shadow lift.
    c.shadow_lift = 0.6f;
    CHECK(h.Convert(ramp, c, graded));
    bool highlights = true, shadows = true, monotonic = true, clipped = false;
    double at_tenth = -1, previous = -1;
    for (unsigned x = 0; x < N; ++x) {
        const double in = Luma(plain, x, N / 2), out = Luma(graded, x, N / 2);
        // The lift meets the identity with slope 1 at 0.75: just below it the change is under half a code (the same byte).
        if (in >= 0.75) highlights = highlights && std::memcmp(&graded[(N / 2 * N + x) * 4], &plain[(N / 2 * N + x) * 4], 4) == 0;
        else if (in > 0) shadows = shadows && (in < 0.5 ? out > in : out >= in);
        else shadows = shadows && out == 0;
        if (graded[(N / 2 * N + x) * 4] == 255 && plain[(N / 2 * N + x) * 4] != 255) clipped = true;
        monotonic = monotonic && out >= previous; previous = out;
        if (at_tenth < 0 && in >= 0.1) at_tenth = out - in;
    }
    std::printf("Picture: shadow lift 0.6 raises Y' 0.10 by %.3f; highlights %s; shadows %s; %s; %s\n", at_tenth,
                highlights ? "bit-identical" : "CHANGED", shadows ? "lifted" : "NOT LIFTED", monotonic ? "monotonic" : "NOT MONOTONIC", clipped ? "CLIPPED" : "unclipped");
    CHECK(highlights && shadows && monotonic && !clipped && at_tenth > 0.07 && at_tenth < 0.12);
    // Saturation 0: grey.
    c = Plain(); c.picture_flags = media::render::kGrade; c.saturation = 0;
    CHECK(h.Convert(colour, c, graded));
    bool grey = true;
    for (unsigned i = 0; i < N * N; ++i) grey = grey && graded[i * 4] == graded[i * 4 + 1] && graded[i * 4 + 1] == graded[i * 4 + 2];
    CHECK(h.Convert(colour, Plain(), plain));
    bool coloured = false;
    for (unsigned i = 0; i < N * N; ++i) coloured = coloured || plain[i * 4] != plain[i * 4 + 2];
    std::printf("Picture: saturation 0 %s (the source %s)\n", grey ? "is grey" : "NOT GREY", coloured ? "is coloured" : "IS NOT COLOURED");
    CHECK(grey && coloured);
    // Micro-motion: a 16 x 16 patch pulsing +-2 codes around a flat 110.
    static Picture flat;
    std::memset(flat.chroma, 128, sizeof(flat.chroma));
    const unsigned frames = 16;
    double pulse_in = 0, pulse_out = 0; bool surround = true;
    Rgba reference_still;
    for (unsigned k = 0; k < frames; ++k) {
        std::memset(flat.luma, 110, sizeof(flat.luma));
        const auto code = static_cast<std::uint8_t>(k & 1 ? 112 : 108);
        for (unsigned y = 24; y < 40; ++y) std::memset(flat.luma + y * N + 24, code, 16);
        auto m = Plain(); m.picture_flags = media::render::kMicro | media::render::kMicroFold | (k == 0 ? media::render::kMicroStart : 0u);
        m.micro_gain = 6.0f;
        Rgba magnified;
        CHECK(h.Convert(flat, Plain(), plain));
        CHECK(h.Convert(flat, m, magnified));
        if (k >= frames - 4) { // Settled: the reference is the pulse's mean.
            const double base = Luma(plain, 8, 8);
            pulse_in += std::fabs(Luma(plain, 32, 32) - base); pulse_out += std::fabs(Luma(magnified, 32, 32) - base);
            for (unsigned y = 0; y < N; ++y) for (unsigned x = 0; x < N; ++x)
                if (x < 24 || x >= 40 || y < 24 || y >= 40) surround = surround && std::memcmp(&magnified[(y * N + x) * 4], &plain[(y * N + x) * 4], 4) == 0;
        }
    }
    const double ratio = pulse_out / pulse_in;
    std::printf("Picture: micro-motion x6 turns a +-2 code pulse (%.2f codes on screen) into %.2f codes (%.1fx); the still surround %s\n",
                pulse_in / 4 * 255, pulse_out / 4 * 255, ratio, surround ? "bit-identical" : "CHANGED");
    CHECK(ratio >= 4.0 && ratio <= 8.0 && pulse_out / 4 * 255 >= 8.0 && surround);
    // A bright bar moving 8 pixels a picture: with and without micro-motion, bit for bit (no ghost, no trail).
    bool ghostless = true;
    for (unsigned k = 0; k < 8; ++k) {
        std::memset(flat.luma, 60, sizeof(flat.luma));
        for (unsigned y = 0; y < N; ++y) std::memset(flat.luma + y * N + 8 * k, 200, 8);
        auto m = Plain(); m.picture_flags = media::render::kMicro | media::render::kMicroFold | (k == 0 ? media::render::kMicroStart : 0u);
        m.micro_gain = 8.0f;
        Rgba magnified;
        CHECK(h.Convert(flat, Plain(), plain));
        CHECK(h.Convert(flat, m, magnified));
        ghostless = ghostless && magnified == plain;
    }
    std::printf("Picture: a bar moving 8 px per picture with micro-motion x8: %s\n", ghostless ? "bit-identical, no ghost" : "GHOSTED");
    CHECK(ghostless);
    // The statistics pass: the brightest sample, then moving blocks for the same picture again (none) and a changed one (all).
    std::uint32_t words[media::render::ColorPipeline::kStatsBytes / 4]{};
    CHECK(h.Convert(ramp, Plain(), plain, words));
    const unsigned brightest = words[8], blocks = words[7] - 0; (void)blocks;
    std::uint32_t again[media::render::ColorPipeline::kStatsBytes / 4]{};
    CHECK(h.Convert(ramp, Plain(), plain, again));
    std::memset(flat.luma, 230, sizeof(flat.luma));
    std::uint32_t changed[media::render::ColorPipeline::kStatsBytes / 4]{};
    CHECK(h.Convert(flat, Plain(), plain, changed));
    const unsigned samples = again[7] - words[7]; // This picture's share of the running count: 8 x 8 = 64 blocks.
    const unsigned expected = (16 + 60 * 219 / (N - 1)) * 1023 / 255; // The sample of the last block column (x = 60).
    std::printf("Picture: statistics: brightest %u (expected about %u), moving blocks %u of %u for the same picture, %u of %u changed\n",
                brightest, expected, again[9], samples, changed[9], changed[7] - again[7]);
    CHECK(brightest + 3 >= expected && brightest <= expected + 3 && samples == 64 && again[9] == 0 && changed[9] == 64);
    // The cost at 1080p: a 1920 x 1080 noise picture converted 1:1, 24 times plain and 24 times with shadow lift, saturation and
    // micro-motion (folding), GPU timestamps around each conversion; medians.
    {
        constexpr unsigned W = 1920, H = 1080, R = 24, kModes = 4; // Plain, controls, micro-motion, both.
        auto* device = h.gpu.device;
        D3D12_HEAP_PROPERTIES props{}; props.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = W; desc.Height = H;
        desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = DXGI_FORMAT_NV12; desc.SampleDesc.Count = 1;
        ID3D12Resource *big = nullptr, *target = nullptr, *upload = nullptr, *stamps = nullptr;
        ID3D12DescriptorHeap* views = nullptr; ID3D12QueryHeap* queries = nullptr;
        bool ready = SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&big)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT planes[2]{}; UINT rows[2]{}; UINT64 sizes[2]{}, total = 0;
        if (ready) device->GetCopyableFootprints(&desc, 0, 2, 0, planes, rows, sizes, &total);
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ready = ready && SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&target)));
        ready = ready && (upload = Buffer(device, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ)) != nullptr;
        ready = ready && (stamps = Buffer(device, D3D12_HEAP_TYPE_READBACK, 2 * kModes * R * 8, D3D12_RESOURCE_STATE_COPY_DEST)) != nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ready = ready && SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&views)));
        D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kModes * R, 0};
        ready = ready && SUCCEEDED(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries))) && SUCCEEDED(h.color.PrepareMotion(device, std::uint64_t{W} * H));
        std::uint8_t* mapped = nullptr;
        if (ready && SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
            std::uint32_t state = 11;
            for (unsigned p = 0; p < 2; ++p)
                for (UINT r = 0; r < rows[p]; ++r)
                    for (UINT x = 0; x < W; ++x) { state = state * 1664525u + 1013904223u; mapped[planes[p].Offset + r * planes[p].Footprint.RowPitch + x] = static_cast<std::uint8_t>(p ? 96 + (state >> 26) : 16 + (state >> 24) % 220); }
            upload->Unmap(0, nullptr);
        } else ready = false;
        UINT64 frequency = 0;
        if (ready && SUCCEEDED(h.gpu.queue->GetTimestampFrequency(&frequency))) {
            for (unsigned s = 0; s < 2; ++s) {
                D3D12_TEXTURE_COPY_LOCATION to{big, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; to.SubresourceIndex = s;
                D3D12_TEXTURE_COPY_LOCATION from{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = planes[s];
                h.gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            auto b = Transition(big, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); h.gpu.list->ResourceBarrier(1, &b);
            const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const auto cpu = [&](unsigned i) { auto c = views->GetCPUDescriptorHandleForHeapStart(); c.ptr += static_cast<SIZE_T>(i) * stride; return c; };
            const auto gpu = [&](unsigned i) { auto c = views->GetGPUDescriptorHandleForHeapStart(); c.ptr += static_cast<UINT64>(i) * stride; return c; };
            for (unsigned plane = 0; plane < 2; ++plane) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
                device->CreateShaderResourceView(big, &srv, cpu(plane));
            }
            device->CreateUnorderedAccessView(target, nullptr, nullptr, cpu(2));
            ID3D12DescriptorHeap* heaps[]{views}; h.gpu.list->SetDescriptorHeaps(1, heaps);
            D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = target;
            for (unsigned mode = 0; mode < kModes; ++mode)
                for (unsigned r = 0; r < R; ++r) {
                    ColorConstants k{W, H, W, H, 0, 0, W, H, 1, 0, 0, 1, 1, 0, 1000.0f, 203.0f, 0, 0.0f, 0.02f, 0, 0.0f, 0, 0};
                    k.contrast = 1; k.saturation = 1; k.gamma = 1; k.micro_gain = 1;
                    if (mode & 1) { k.picture_flags |= media::render::kGrade; k.shadow_lift = 0.6f; k.saturation = 1.2f; }
                    if (mode & 2) { k.picture_flags |= media::render::kMicro | media::render::kMicroFold | (r == 0 ? media::render::kMicroStart : 0u); k.micro_gain = 6.0f; }
                    h.gpu.list->ResourceBarrier(1, &uav);
                    h.gpu.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * (mode * R + r));
                    h.color.Record(h.gpu.list, gpu(0), gpu(2), k);
                    h.gpu.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * (mode * R + r) + 1);
                }
            h.gpu.list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * kModes * R, stamps, 0);
            ready = h.gpu.Submit();
        } else ready = false;
        double median[kModes]{};
        if (ready && SUCCEEDED(stamps->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
            const auto* t = reinterpret_cast<const std::uint64_t*>(mapped);
            for (unsigned mode = 0; mode < kModes; ++mode) {
                double times[R];
                for (unsigned r = 0; r < R; ++r) times[r] = static_cast<double>(t[2 * (mode * R + r) + 1] - t[2 * (mode * R + r)]) * 1000.0 / static_cast<double>(frequency);
                std::sort(times, times + R); median[mode] = times[R / 2];
            }
            const D3D12_RANGE none{0, 0}; stamps->Unmap(0, &none);
        }
        std::printf("Picture: 1080p conversion %.3f ms plain; shadow lift and saturation %+.3f ms, micro-motion %+.3f ms, both %+.3f ms (GPU medians of %u)\n",
                    median[0], median[1] - median[0], median[2] - median[0], median[3] - median[0], R);
        CHECK(ready && median[0] > 0 && median[3] > 0);
        ReleaseCom(queries); ReleaseCom(views); ReleaseCom(stamps); ReleaseCom(upload); ReleaseCom(target); ReleaseCom(big);
    }
    return 0;
#undef CHECK
}
}
