// Downscale prefilter and dither of the production colour pipeline (docs/picture.md, "Downscale prefilter and dither"),
// on synthetic NV12/P010 test input (never decoded video), read back as RGBA8:
// - aliasing: a sinusoid above the output's Nyquist frequency, downscaled 2x, 2.5x and 3x: the RMS left in the output
//   with and without the prefilter; a sinusoid well below it keeps its amplitude;
// - crop safety: a picture inside a surface whose padding is poison (white luma, extreme chroma) converts bit for bit as
//   the same picture alone, at every factor and with the prefilter;
// - dither: a dark 10-bit ramp at 1:1, against the same conversion into a float target: per pixel within one LSB, the
//   column means much closer to the unquantized values (banding), deterministic, and 8-bit sources untouched;
// - GPU time of a 4K Main10 PQ conversion to 1280x720 and 1920x1080, with and without the prefilter and dither.
#include "media/render/ColorPipeline.hpp"
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
constexpr double kPi = 3.14159265358979323846;

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
// A planar 4:2:0 picture (setup only): luma and interleaved chroma samples, 8-bit (NV12) or 10-bit (P010, value << 6).
struct Picture { unsigned w = 0, h = 0; bool ten = false; std::vector<std::uint16_t> luma, chroma; };
struct Source { ID3D12Resource* texture = nullptr; unsigned w = 0, h = 0; bool ten = false; };
Source Upload(Gpu& gpu, const Picture& p, std::vector<ID3D12Resource*>& keep) {
    Source s{Texture(gpu.device, p.w, p.h, p.ten ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE), p.w, p.h, p.ten};
    if (!s.texture) return s;
    const auto desc = s.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2]{}; UINT rows[2]{}; UINT64 sizes[2]{}, total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 2, 0, layouts, rows, sizes, &total);
    auto* staging = Buffer(gpu.device, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!staging) { ReleaseCom(s.texture); return s; }
    keep.push_back(staging);
    std::uint8_t* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) { ReleaseCom(s.texture); return s; }
    const unsigned bytes = p.ten ? 2u : 1u;
    for (UINT r = 0; r < rows[0]; ++r)
        for (unsigned x = 0; x < p.w; ++x) {
            const auto v = p.luma[static_cast<std::size_t>(r) * p.w + x];
            auto* at = mapped + layouts[0].Offset + r * layouts[0].Footprint.RowPitch + x * bytes;
            if (p.ten) { const std::uint16_t word = static_cast<std::uint16_t>(v << 6); std::memcpy(at, &word, 2); } else *at = static_cast<std::uint8_t>(v);
        }
    for (UINT r = 0; r < rows[1]; ++r)
        for (unsigned x = 0; x < p.w; ++x) {
            const auto v = p.chroma[static_cast<std::size_t>(r) * p.w + x];
            auto* at = mapped + layouts[1].Offset + r * layouts[1].Footprint.RowPitch + x * bytes;
            if (p.ten) { const std::uint16_t word = static_cast<std::uint16_t>(v << 6); std::memcpy(at, &word, 2); } else *at = static_cast<std::uint8_t>(v);
        }
    staging->Unmap(0, nullptr);
    for (unsigned plane = 0; plane < 2; ++plane) {
        D3D12_TEXTURE_COPY_LOCATION to{s.texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; to.SubresourceIndex = plane;
        D3D12_TEXTURE_COPY_LOCATION from{staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = layouts[plane];
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    const auto barrier = Transition(s.texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.list->ResourceBarrier(1, &barrier);
    return s;
}
Picture Flat(unsigned w, unsigned h, bool ten, unsigned y, unsigned cb = 128, unsigned cr = 128) {
    Picture p{w, h, ten, std::vector<std::uint16_t>(static_cast<std::size_t>(w) * h, static_cast<std::uint16_t>(y)), std::vector<std::uint16_t>(static_cast<std::size_t>(w) * (h / 2))};
    for (std::size_t i = 0; i < p.chroma.size(); i += 2) { p.chroma[i] = static_cast<std::uint16_t>(cb); p.chroma[i + 1] = static_cast<std::uint16_t>(cr); }
    return p;
}
// A vertical-stripe sinusoid of period `period` source pixels around luma 128, amplitude 80 (8-bit limited range).
Picture Stripes(unsigned w, unsigned h, double period) {
    auto p = Flat(w, h, false, 128);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) p.luma[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint16_t>(std::lround(128 + 80 * std::sin(2 * kPi * (x + 0.5) / period)));
    return p;
}
// `inner` placed at (ox, oy) in a w x h surface whose padding is poison: white luma, extreme chroma.
Picture Padded(const Picture& inner, unsigned w, unsigned h, unsigned ox, unsigned oy) {
    auto p = Flat(w, h, inner.ten, inner.ten ? 1023 : 255, 0, inner.ten ? 1023 : 255);
    for (unsigned y = 0; y < inner.h; ++y)
        for (unsigned x = 0; x < inner.w; ++x) p.luma[static_cast<std::size_t>(y + oy) * w + x + ox] = inner.luma[static_cast<std::size_t>(y) * inner.w + x];
    for (unsigned y = 0; y < inner.h / 2; ++y)
        for (unsigned x = 0; x < inner.w; ++x) p.chroma[static_cast<std::size_t>(y + oy / 2) * w + x + ox] = inner.chroma[static_cast<std::size_t>(y) * inner.w + x];
    return p;
}

struct Harness {
    Gpu gpu; media::render::ColorPipeline color;
    ID3D12DescriptorHeap* heap = nullptr; UINT stride = 0;
    ID3D12Resource* target = nullptr; ID3D12Resource* readback = nullptr; ID3D12QueryHeap* queries = nullptr; ID3D12Resource* stamps = nullptr;
    unsigned tw = 0, th = 0; DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes = 0;
    UINT64 frequency = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned i) { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * stride; return h; }
    D3D12_GPU_DESCRIPTOR_HANDLE Handle(unsigned i) { auto h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(i) * stride; return h; }
    bool Target(unsigned w, unsigned h, DXGI_FORMAT f) {
        ReleaseCom(target); ReleaseCom(readback);
        tw = w; th = h; format = f;
        target = Texture(gpu.device, w, h, f, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!target) return false;
        const auto desc = target->GetDesc(); gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        readback = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.device->CreateUnorderedAccessView(target, nullptr, nullptr, Cpu(2));
        return readback != nullptr;
    }
    void Bind(const Source& s) {
        for (unsigned plane = 0; plane < 2; ++plane) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = s.ten ? (plane ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM) : (plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM);
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
            gpu.device->CreateShaderResourceView(s.texture, &srv, Cpu(plane));
        }
    }
    // The crop (l, t)-(r, b) of `s` to the target, with `flags`; transfer 1 (BT.709) unless `pq`. Returns the GPU time.
    double Convert(const Source& s, unsigned l, unsigned t, unsigned r, unsigned b, std::uint32_t flags, bool pq = false, unsigned repeats = 1) {
        Bind(s);
        ColorConstants k{tw, th, s.w, s.h, l, t, r, b, pq ? 9u : 1u, 0, s.ten ? 1u : 0u, pq ? 9u : 1u, pq ? 16u : 1u, 0, 1000.0f, 203.0f,
                         media::render::kBilinear, 0, 0.02f, media::render::kBilinearChroma, 0, 0, flags};
        ID3D12DescriptorHeap* heaps[]{heap}; gpu.list->SetDescriptorHeaps(1, heaps);
        gpu.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (unsigned i = 0; i < repeats; ++i) {
            color.Record(gpu.list, Handle(0), Handle(2), k);
            D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = target; gpu.list->ResourceBarrier(1, &uav);
        }
        gpu.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        gpu.list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamps, 0);
        auto barrier = Transition(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION to{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); gpu.list->ResourceBarrier(1, &barrier);
        if (!gpu.Submit()) { CHECK(false); return 0; }
        std::uint64_t* t2 = nullptr; double ms = 0;
        if (SUCCEEDED(stamps->Map(0, nullptr, reinterpret_cast<void**>(&t2)))) {
            ms = static_cast<double>(t2[1] - t2[0]) * 1000.0 / static_cast<double>(frequency) / repeats;
            const D3D12_RANGE none{0, 0}; stamps->Unmap(0, &none);
        }
        return ms;
    }
    // The target's first channel (R) as values in [0, 1].
    void Red(std::vector<double>& out) {
        out.assign(static_cast<std::size_t>(tw) * th, 0);
        std::uint8_t* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) { CHECK(false); return; }
        for (unsigned y = 0; y < th; ++y)
            for (unsigned x = 0; x < tw; ++x) {
                const auto* row = mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
                if (format == DXGI_FORMAT_R8G8B8A8_UNORM) out[static_cast<std::size_t>(y) * tw + x] = row[x * 4] / 255.0;
                else { float v = 0; std::memcpy(&v, row + x * 16, 4); out[static_cast<std::size_t>(y) * tw + x] = v; }
            }
        const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
    }
    void Raw(std::vector<std::uint8_t>& out) {
        out.assign(static_cast<std::size_t>(tw) * th * 4, 0);
        std::uint8_t* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) { CHECK(false); return; }
        for (unsigned y = 0; y < th; ++y) std::memcpy(out.data() + static_cast<std::size_t>(y) * tw * 4, mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch, tw * 4);
        const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
    }
};
double Rms(const std::vector<double>& v, unsigned w, unsigned h, unsigned margin) {
    double sum = 0, squares = 0; unsigned n = 0;
    for (unsigned y = margin; y + margin < h; ++y) for (unsigned x = margin; x + margin < w; ++x) { const auto a = v[static_cast<std::size_t>(y) * w + x]; sum += a; squares += a * a; ++n; }
    const double mean = sum / n; return std::sqrt(std::max(squares / n - mean * mean, 0.0));
}
}

int main() {
    Harness h;
    IDXGIFactory6* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        for (UINT i = 0; !h.gpu.device; ++i) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) (void)D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&h.gpu.device));
            ReleaseCom(adapter);
        }
    }
    ReleaseCom(factory);
    if (!h.gpu.device) { std::puts("SKIP no hardware D3D12 device"); return 77; }
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = h.gpu.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&h.gpu.queue));
    if (SUCCEEDED(hr)) hr = h.gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&h.gpu.fence));
    if (SUCCEEDED(hr)) hr = h.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&h.gpu.allocator));
    if (SUCCEEDED(hr)) hr = h.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.gpu.allocator, nullptr, IID_PPV_ARGS(&h.gpu.list));
    if (SUCCEEDED(hr)) hr = h.color.Create(h.gpu.device);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (SUCCEEDED(hr)) hr = h.gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&h.heap));
    D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
    if (SUCCEEDED(hr)) hr = h.gpu.device->CreateQueryHeap(&query, IID_PPV_ARGS(&h.queries));
    if (SUCCEEDED(hr)) hr = h.gpu.queue->GetTimestampFrequency(&h.frequency);
    h.stamps = Buffer(h.gpu.device, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
    if (FAILED(hr) || !h.stamps) { std::printf("FAIL setup %08lx\n", static_cast<unsigned long>(hr)); return 1; }
    h.stride = h.gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::vector<ID3D12Resource*> keep;
    using media::render::kAreaPrefilter; using media::render::kDither;

    // ---- Aliasing: 2x, 2.5x and 3x from 960x540, a sinusoid of period 2.5 source pixels (above every output Nyquist) ----
    constexpr unsigned W = 960, H = 540;
    const auto fine = Upload(h.gpu, Stripes(W, H, 2.5), keep), coarse = Upload(h.gpu, Stripes(W, H, 48.0), keep);
    CHECK(h.gpu.Submit() && fine.texture && coarse.texture);
    struct Factor { const char* name; unsigned w, h; double box; } factors[]{{"2x", 480, 270, 0}, {"2.5x", 384, 216, 0}, {"3x", 320, 180, 0}};
    std::vector<double> red;
    for (const auto& f : factors) {
        CHECK(h.Target(f.w, f.h, DXGI_FORMAT_R8G8B8A8_UNORM));
        (void)h.Convert(fine, 0, 0, W, H, 0); h.Red(red); const double before = Rms(red, f.w, f.h, 2);
        (void)h.Convert(fine, 0, 0, W, H, kAreaPrefilter); h.Red(red); const double after = Rms(red, f.w, f.h, 2);
        // The ideal box over the footprint attenuates the sinusoid by |sinc(span / period)|.
        const double span = static_cast<double>(W) / f.w, x = kPi * span / 2.5, box = std::fabs(std::sin(x) / x);
        (void)h.Convert(coarse, 0, 0, W, H, 0); h.Red(red); const double pass_before = Rms(red, f.w, f.h, 2);
        (void)h.Convert(coarse, 0, 0, W, H, kAreaPrefilter); h.Red(red); const double pass_after = Rms(red, f.w, f.h, 2);
        std::printf("Aliasing %s (period 2.5 source px): RMS %.4f -> %.4f (x%.2f; ideal box leaves %.3f of the amplitude); passband (period 48): %.4f -> %.4f\n",
                    f.name, before, after, after > 0 ? before / after : 0.0, box, pass_before, pass_after);
        // At exactly 2x bilinear already is the 2x2 box. Elsewhere the area average of the texels must remove much of what
        // the single bilinear sample aliases (the continuous box's figure is not reachable: the texels are the signal).
        if (std::strcmp(f.name, "2x") == 0) CHECK(std::fabs(after - before) < 0.01);
        else if (std::strcmp(f.name, "2.5x") == 0) CHECK(after < before / 1.3);
        else CHECK(after < before / 3);
        CHECK(pass_after > 0.9 * pass_before); // What the output can show keeps its amplitude.
    }

    // ---- Crop safety: the picture inside a poisoned surface converts as itself alone, at every factor ----
    {
        auto inner = Stripes(W, H, 7.3);
        for (unsigned y = 0; y < H / 2; ++y) for (unsigned x = 0; x < W; x += 2) { inner.chroma[static_cast<std::size_t>(y) * W + x] = static_cast<std::uint16_t>(64 + (x * 7 + y * 3) % 128); inner.chroma[static_cast<std::size_t>(y) * W + x + 1] = static_cast<std::uint16_t>(80 + (x * 5 + y * 11) % 96); }
        const auto alone = Upload(h.gpu, inner, keep), padded = Upload(h.gpu, Padded(inner, W + 32, H + 24, 16, 8), keep);
        CHECK(h.gpu.Submit() && alone.texture && padded.texture);
        std::vector<std::uint8_t> a, b; unsigned identical = 0, cases = 0;
        const unsigned sizes[][2]{{480, 270}, {384, 216}, {320, 180}, {700, 394}, {240, 135}, {213, 120}};
        for (const auto& size : sizes)
            for (const std::uint32_t flags : {std::uint32_t{0}, std::uint32_t{kAreaPrefilter}, std::uint32_t{kAreaPrefilter | kDither}}) {
                CHECK(h.Target(size[0], size[1], DXGI_FORMAT_R8G8B8A8_UNORM));
                (void)h.Convert(alone, 0, 0, W, H, flags); h.Raw(a);
                (void)h.Convert(padded, 16, 8, 16 + W, 8 + H, flags); h.Raw(b);
                ++cases; if (a == b) ++identical;
            }
        std::printf("Crop safety under poison padding: %u of %u conversions bit-identical (6 sizes x 3 modes)\n", identical, cases);
        CHECK(identical == cases);
    }

    // ---- Dither: a dark 10-bit ramp at 1:1 ----
    {
        constexpr unsigned RW = 1024, RH = 64;
        auto ramp = Flat(RW, RH, true, 64, 512, 512);
        for (unsigned y = 0; y < RH; ++y) for (unsigned x = 0; x < RW; ++x) ramp.luma[static_cast<std::size_t>(y) * RW + x] = static_cast<std::uint16_t>(64 + x * 64 / RW); // 64 codes: 16 per 8-bit step.
        const auto source = Upload(h.gpu, ramp, keep);
        auto flat8 = Flat(RW, RH, false, 30); for (unsigned x = 0; x < RW; ++x) for (unsigned y = 0; y < RH; ++y) flat8.luma[static_cast<std::size_t>(y) * RW + x] = static_cast<std::uint16_t>(16 + x * 32 / RW);
        const auto source8 = Upload(h.gpu, flat8, keep);
        CHECK(h.gpu.Submit() && source.texture && source8.texture);
        std::vector<double> ideal, plain, dithered;
        CHECK(h.Target(RW, RH, DXGI_FORMAT_R32G32B32A32_FLOAT)); (void)h.Convert(source, 0, 0, RW, RH, 0); h.Red(ideal);
        CHECK(h.Target(RW, RH, DXGI_FORMAT_R8G8B8A8_UNORM));
        (void)h.Convert(source, 0, 0, RW, RH, 0); h.Red(plain);
        (void)h.Convert(source, 0, 0, RW, RH, kDither); h.Red(dithered);
        std::vector<std::uint8_t> once, twice; h.Raw(once); (void)h.Convert(source, 0, 0, RW, RH, kDither); h.Raw(twice);
        double worst = 0, banding_plain = 0, banding_dither = 0; unsigned runs_plain = 0, runs_dither = 0;
        for (unsigned x = 0; x < RW; ++x) {
            double mp = 0, md = 0, mi = 0;
            for (unsigned y = 0; y < RH; ++y) {
                const auto i = static_cast<std::size_t>(y) * RW + x;
                worst = std::max(worst, std::fabs(dithered[i] - ideal[i]) * 255); mp += plain[i]; md += dithered[i]; mi += ideal[i];
            }
            mp /= RH; md /= RH; mi /= RH;
            banding_plain += (mp - mi) * (mp - mi) * 65025; banding_dither += (md - mi) * (md - mi) * 65025;
            if (x && plain[x] != plain[x - 1]) ++runs_plain;
            if (x && dithered[x] != dithered[x - 1]) ++runs_dither;
        }
        banding_plain = std::sqrt(banding_plain / RW); banding_dither = std::sqrt(banding_dither / RW);
        std::printf("Dither (10-bit ramp, 64 codes over %u px): column-mean error vs unquantized %.3f LSB plain -> %.3f LSB dithered; worst pixel %.3f LSB from unquantized; value changes along a row %u -> %u\n",
                    RW, banding_plain, banding_dither, worst, runs_plain, runs_dither);
        CHECK(worst <= 1.0 + 1e-6 && banding_dither < banding_plain / 2.5 && once == twice && runs_dither > 4 * runs_plain);
        // 8-bit sources are not dithered: the flag changes nothing.
        std::vector<std::uint8_t> eight, eight_dither;
        (void)h.Convert(source8, 0, 0, RW, RH, 0); h.Raw(eight); (void)h.Convert(source8, 0, 0, RW, RH, kDither); h.Raw(eight_dither);
        CHECK(eight == eight_dither);
        // Pure black and white stay exact.
        const auto black = Upload(h.gpu, Flat(256, 64, true, 64, 512, 512), keep), white = Upload(h.gpu, Flat(256, 64, true, 940, 512, 512), keep);
        CHECK(h.gpu.Submit());
        CHECK(h.Target(256, 64, DXGI_FORMAT_R8G8B8A8_UNORM));
        std::vector<std::uint8_t> k; (void)h.Convert(black, 0, 0, 256, 64, kDither); h.Raw(k);
        bool exact = true; for (std::size_t i = 0; i < k.size(); i += 4) exact = exact && k[i] == 0 && k[i + 1] == 0 && k[i + 2] == 0;
        (void)h.Convert(white, 0, 0, 256, 64, kDither); h.Raw(k);
        for (std::size_t i = 0; i < k.size(); i += 4) exact = exact && k[i] == 255 && k[i + 1] == 255 && k[i + 2] == 255;
        CHECK(exact);
    }

    // ---- GPU time: 4K Main10 PQ to 1280x720 and 1920x1080 ----
    {
        auto big = Flat(3840, 2160, true, 400, 480, 560);
        for (unsigned y = 0; y < 2160; ++y) for (unsigned x = 0; x < 3840; ++x) big.luma[static_cast<std::size_t>(y) * 3840 + x] = static_cast<std::uint16_t>(64 + (x * 7 + y * 13) % 876);
        const auto source = Upload(h.gpu, big, keep);
        CHECK(h.gpu.Submit() && source.texture);
        const std::pair<unsigned, unsigned> outputs[]{{1280u, 720u}, {1920u, 1080u}};
        for (const auto& size : outputs) {
            CHECK(h.Target(size.first, size.second, DXGI_FORMAT_R8G8B8A8_UNORM));
            // The four modes interleaved within each round, so a change of GPU clock or load affects them alike.
            const std::uint32_t modes[4]{0, kAreaPrefilter, kDither, kAreaPrefilter | kDither};
            std::vector<double> runs[4];
            for (unsigned r = 0; r < 15; ++r)
                for (unsigned mode = 0; mode < 4; ++mode) runs[mode].push_back(h.Convert(source, 0, 0, 3840, 2160, modes[mode], true, 4));
            double t[4]{};
            for (unsigned mode = 0; mode < 4; ++mode) { std::sort(runs[mode].begin(), runs[mode].end()); t[mode] = runs[mode][7]; }
            std::printf("GPU time, 4K Main10 PQ -> %ux%u (detect, peak, tone map, write), median of 15: %.2f ms bilinear, %.2f ms prefilter, %.2f ms dither, %.2f ms both\n",
                        size.first, size.second, t[0], t[1], t[2], t[3]);
        }
    }
    for (auto* r : keep) ReleaseCom(r);
    std::printf("Downscale prefilter / dither: %d failures\n", failures);
    return failures ? 1 : 0;
}
