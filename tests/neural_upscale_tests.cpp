// NeuralSuperRes.hlsl against the CPU reference (media::video::NeuralUpscaleReference) and a hand-derived analytic
// fixture, on synthetic luma planes only (never decoded video): NV12 and P010 planes, R32F and R16F outputs, crops that
// are not tile multiples and start at an offset, no writes outside the 2x crop, finite and clamped output for extreme
// input and adversarial weights, zero allocations when recording, and GPU timestamps at 720p and 1080p.
// Temporal tile reuse: bit-identical to full evaluation over static, partial-motion, full-motion, cut, crop-change and
// format-change sequences (NV12/P010, R32F/R16F); exactly the tiles whose 20x20 neighbourhood changed are evaluated
// (sentinel test); and its GPU time on static and semi-static 720p/1080p frames.
#include "media/render/NeuralUpscalePass.hpp"
#include "media/render/NeuralWeights.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using media::video::NeuralModel;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
struct Gpu {
    ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr; ID3D12DescriptorHeap* heap = nullptr;
    ID3D12QueryHeap* queries = nullptr; ID3D12Resource* timestamps = nullptr; HANDLE event = nullptr; std::uint64_t value = 0; UINT stride = 0;
    bool Init() {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        D3D12_DESCRIPTOR_HEAP_DESC h{}; h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; h.NumDescriptors = 8; h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        D3D12_QUERY_HEAP_DESC t{}; t.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; t.Count = 2;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bool ok = event && SUCCEEDED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
                  SUCCEEDED(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator))) &&
                  SUCCEEDED(device->CreateCommandList(0, q.Type, allocator, nullptr, IID_PPV_ARGS(&list))) &&
                  SUCCEEDED(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap))) && SUCCEEDED(device->CreateQueryHeap(&t, IID_PPV_ARGS(&queries)));
        stride = device->GetDescriptorHandleIncrementSize(h.Type);
        return ok;
    }
    bool Submit() {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
        return SUCCEEDED(queue->Signal(fence, ++value)) && SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
               WaitForSingleObject(event, 20000) == WAIT_OBJECT_0 && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned i) const { auto c = heap->GetCPUDescriptorHandleForHeapStart(); c.ptr += SIZE_T{i} * stride; return c; }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu_(unsigned i) const { auto g = heap->GetGPUDescriptorHandleForHeapStart(); g.ptr += UINT64{i} * stride; return g; }
    ~Gpu() { Release(timestamps); Release(queries); Release(heap); Release(list); Release(allocator); Release(fence); Release(queue); Release(device); if (event) CloseHandle(event); }
};
ID3D12Resource* Resource(ID3D12Device* device, D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type; ID3D12Resource* r = nullptr;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))) ? r : nullptr;
}
D3D12_RESOURCE_DESC Texture(unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format; d.SampleDesc.Count = 1; d.Flags = flags; return d;
}
D3D12_RESOURCE_DESC Buffer(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b}; return x;
}
// A 4:2:0 frame (NV12 or P010) whose luma plane holds `codes` (8-bit codes; P010 stores code * 4 << 6); chroma neutral.
struct Frame { ID3D12Resource* texture = nullptr; std::vector<ID3D12Resource*> keep; ~Frame() { Release(texture); for (auto* r : keep) Release(r); } };
bool MakeFrame(Gpu& g, Frame& f, unsigned w, unsigned h, bool ten_bit, const std::vector<unsigned>& codes, unsigned slot = 0) {
    const auto desc = Texture(w, h, ten_bit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12);
    f.texture = Resource(g.device, D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST); if (!f.texture) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2]{}; UINT rows[2]{}; UINT64 sizes[2]{}, total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 2, 0, layouts, rows, sizes, &total);
    auto* upload = Resource(g.device, D3D12_HEAP_TYPE_UPLOAD, Buffer(total), D3D12_RESOURCE_STATE_GENERIC_READ); if (!upload) return false;
    f.keep.push_back(upload);
    std::uint8_t* data = nullptr; if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&data)))) return false;
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        auto* row = data + layouts[0].Offset + std::size_t{y} * layouts[0].Footprint.RowPitch;
        if (ten_bit) { const std::uint16_t v = static_cast<std::uint16_t>((codes[y * w + x] * 4) << 6); std::memcpy(row + x * 2, &v, 2); }
        else row[x] = static_cast<std::uint8_t>(codes[y * w + x]);
    }
    for (unsigned y = 0; y < (h + 1) / 2; ++y) for (unsigned x = 0; x < (w + 1) / 2; ++x) {
        auto* row = data + layouts[1].Offset + std::size_t{y} * layouts[1].Footprint.RowPitch;
        if (ten_bit) { const std::uint16_t v[2]{512u << 6, 512u << 6}; std::memcpy(row + x * 4, v, 4); } else { row[x * 2] = row[x * 2 + 1] = 128; }
    }
    upload->Unmap(0, nullptr);
    for (UINT plane = 0; plane < 2; ++plane) {
        D3D12_TEXTURE_COPY_LOCATION to{f.texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; to.SubresourceIndex = plane;
        D3D12_TEXTURE_COPY_LOCATION from{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = layouts[plane];
        g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    auto b = Transition(f.texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); g.list->ResourceBarrier(1, &b);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = 0;
    g.device->CreateShaderResourceView(f.texture, &srv, g.Cpu(slot));
    return g.Submit();
}
// Runs the pass over the crop into a (2 cw + 2 margin) x (2 ch + 2 margin) output pre-filled with a sentinel, and reads it
// back (synthetic data). Returns the whole output including the margin.
constexpr float kSentinel = -7.0f; constexpr unsigned kMargin = 3;
bool Run(Gpu& g, media::render::NeuralUpscalePass& pass, unsigned left, unsigned top, unsigned right, unsigned bottom, bool half,
         std::vector<float>& out, unsigned& ow, unsigned& oh, bool ten_bit_view = false) {
    ow = 2 * (right - left) + kMargin; oh = 2 * (bottom - top) + kMargin;
    const auto format = half ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT; const unsigned bytes = half ? 2 : 4;
    const auto desc = Texture(ow, oh, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* output = Resource(g.device, D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 total = 0; g.device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &total);
    ID3D12Resource* upload = Resource(g.device, D3D12_HEAP_TYPE_UPLOAD, Buffer(total), D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* readback = Resource(g.device, D3D12_HEAP_TYPE_READBACK, Buffer(total), D3D12_RESOURCE_STATE_COPY_DEST);
    bool ok = output && upload && readback;
    std::uint8_t* data = nullptr;
    if (ok && SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&data)))) {
        const std::uint16_t sentinel_half = 0xc700; // -7.0 in binary16.
        for (unsigned y = 0; y < oh; ++y) for (unsigned x = 0; x < ow; ++x) {
            auto* p = data + layout.Offset + std::size_t{y} * layout.Footprint.RowPitch + std::size_t{x} * bytes;
            if (half) std::memcpy(p, &sentinel_half, 2); else std::memcpy(p, &kSentinel, 4);
        }
        upload->Unmap(0, nullptr);
    }
    if (ok) {
        D3D12_TEXTURE_COPY_LOCATION to{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, from{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        from.PlacedFootprint = layout; g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        auto b = Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); g.list->ResourceBarrier(1, &b);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.device->CreateUnorderedAccessView(output, nullptr, &uav, g.Cpu(1));
        ID3D12DescriptorHeap* heaps[]{g.heap}; g.list->SetDescriptorHeaps(1, heaps);
        pass.Invalidate(); // A new output texture: nothing in it to reuse.
        pass.Record(g.list, g.Gpu_(0), g.Gpu_(1), left, top, right, bottom, ten_bit_view);
        b = Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE); g.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint = layout; g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        ok = g.Submit();
    }
    if (ok && SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)))) {
        out.resize(std::size_t{ow} * oh);
        for (unsigned y = 0; y < oh; ++y) for (unsigned x = 0; x < ow; ++x) {
            const auto* p = data + layout.Offset + std::size_t{y} * layout.Footprint.RowPitch + std::size_t{x} * bytes;
            if (half) { // binary16 -> float
                std::uint16_t h = 0; std::memcpy(&h, p, 2);
                const unsigned sign = h >> 15, exponent = (h >> 10) & 31, mantissa = h & 1023;
                float v = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24) : exponent == 31 ? (mantissa ? NAN : INFINITY) :
                          std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
                out[std::size_t{y} * ow + x] = sign ? -v : v;
            } else std::memcpy(&out[std::size_t{y} * ow + x], p, 4);
        }
        readback->Unmap(0, nullptr);
    } else ok = false;
    Release(readback); Release(upload); Release(output);
    return ok;
}
std::uint32_t seed = 12345;
float Random() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }
NeuralModel RandomModel(float scale) {
    NeuralModel m{}; auto* w = reinterpret_cast<float*>(&m);
    for (unsigned i = 0; i < sizeof(m) / 4; ++i) w[i] = (Random() * 2 - 1) * scale;
    return m;
}
// Compares one run with the CPU reference; checks the sentinel margin, finiteness and the [0, 1] range.
void Compare(const char* name, const std::vector<float>& gpu, unsigned ow, unsigned oh, const std::vector<unsigned>& codes, unsigned w,
             unsigned left, unsigned top, unsigned right, unsigned bottom, bool ten_bit, const NeuralModel& model, float tolerance) {
    const unsigned cw = right - left, ch = bottom - top;
    std::vector<float> luma(std::size_t{cw} * ch), ref(std::size_t{4} * cw * ch);
    for (unsigned y = 0; y < ch; ++y) for (unsigned x = 0; x < cw; ++x) {
        const unsigned code = codes[(top + y) * w + left + x];
        luma[std::size_t{y} * cw + x] = ten_bit ? static_cast<float>((code * 4) << 6) / 65535.0f : code / 255.0f;
    }
    media::video::NeuralUpscaleReference({luma.data(), cw, ch, cw}, model, ref.data(), std::size_t{2} * cw);
    double worst = 0; unsigned outside = 0, invalid = 0;
    for (unsigned y = 0; y < oh; ++y) for (unsigned x = 0; x < ow; ++x) {
        const float v = gpu[std::size_t{y} * ow + x];
        if (x >= 2 * cw || y >= 2 * ch) { outside += v != kSentinel; continue; }
        if (!std::isfinite(v) || v < 0 || v > 1) ++invalid;
        worst = (std::max)(worst, static_cast<double>(std::fabs(v - ref[std::size_t{y} * 2 * cw + x])));
    }
    std::printf("  %-44s max |GPU - CPU| %.2e, outside writes %u, invalid %u\n", name, worst, outside, invalid);
    CHECK(worst <= tolerance && outside == 0 && invalid == 0);
}
}
namespace {
// A persistent output for sequences (temporal reuse): it stays in UNORDERED_ACCESS between Records, as the renderer's
// 2x texture does. Fill and Read work on raw bit patterns (R16F in the low 16 bits).
struct Output {
    ID3D12Resource *texture = nullptr, *upload = nullptr, *readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; unsigned w = 0, h = 0, bytes = 4, slot = 0;
    bool Init(Gpu& g, unsigned width, unsigned height, bool half, unsigned descriptor) {
        w = width; h = height; bytes = half ? 2 : 4; slot = descriptor;
        const auto format = half ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
        const auto desc = Texture(w, h, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        UINT64 total = 0; g.device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &total);
        texture = Resource(g.device, D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        upload = Resource(g.device, D3D12_HEAP_TYPE_UPLOAD, Buffer(total), D3D12_RESOURCE_STATE_GENERIC_READ);
        readback = Resource(g.device, D3D12_HEAP_TYPE_READBACK, Buffer(total), D3D12_RESOURCE_STATE_COPY_DEST);
        if (!texture || !upload || !readback) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.device->CreateUnorderedAccessView(texture, nullptr, &uav, g.Cpu(slot));
        return true;
    }
    bool Fill(Gpu& g, std::uint32_t bits) {
        std::uint8_t* data = nullptr; if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&data)))) return false;
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) std::memcpy(data + layout.Offset + std::size_t{y} * layout.Footprint.RowPitch + std::size_t{x} * bytes, &bits, bytes);
        upload->Unmap(0, nullptr);
        auto b = Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST); g.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION to{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, from{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        from.PlacedFootprint = layout; g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        b = Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); g.list->ResourceBarrier(1, &b);
        return g.Submit();
    }
    bool Read(Gpu& g, std::vector<std::uint32_t>& out) {
        auto b = Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE); g.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint = layout; g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b = Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); g.list->ResourceBarrier(1, &b);
        if (!g.Submit()) return false;
        std::uint8_t* data = nullptr; if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)))) return false;
        out.assign(std::size_t{w} * h, 0);
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) std::memcpy(&out[std::size_t{y} * w + x], data + layout.Offset + std::size_t{y} * layout.Footprint.RowPitch + std::size_t{x} * bytes, bytes);
        readback->Unmap(0, nullptr);
        return true;
    }
    ~Output() { Release(readback); Release(upload); Release(texture); }
};
// One Record of `pass` from the luma SRV at `luma` into `o`, executed.
bool Step(Gpu& g, media::render::NeuralUpscalePass& pass, const Output& o, unsigned luma, unsigned l, unsigned t, unsigned r, unsigned b, bool ten_bit) {
    ID3D12DescriptorHeap* heaps[]{g.heap}; g.list->SetDescriptorHeaps(1, heaps);
    pass.Record(g.list, g.Gpu_(luma), g.Gpu_(o.slot), l, t, r, b, ten_bit);
    D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = o.texture; g.list->ResourceBarrier(1, &uav);
    return g.Submit();
}
// Synthetic luma with detail everywhere (hashed noise over edges and bars), limited-range codes; `seed` picks the picture.
unsigned Detail(unsigned x, unsigned y, unsigned picture) {
    std::uint32_t v = (x * 73856093u) ^ (y * 19349663u) ^ (picture * 83492791u); v ^= v >> 13; v *= 0x5bd1e995u; v ^= v >> 15;
    const unsigned edge = (x + 2 * y + picture * 7) % 37 < 18 ? 60 : 170;
    return std::clamp(edge + v % 40, 16u, 235u);
}
// Frame `t` of a sequence: 0 static, 1 a moving 12x12 square plus a one-code flicker, 2 a global pan, 3 cuts (A, B, A, ...).
std::vector<unsigned> Scene(unsigned w, unsigned h, unsigned kind, unsigned t) {
    std::vector<unsigned> codes(std::size_t{w} * h);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        unsigned v = kind == 2 ? Detail(x + 3 * t, y + 2 * t, 1) : Detail(x, y, kind == 3 && t % 2 ? 2 : 1);
        if (kind == 1 && x >= 20 + 7 * t && x < 32 + 7 * t && y >= 30 + 3 * t && y < 42 + 3 * t) v = 230;
        codes[std::size_t{y} * w + x] = v;
    }
    if (kind == 1 && t % 2) codes[std::size_t{50} * w + 100] += 1; // One code, one texel: its neighbourhood re-evaluates.
    return codes;
}
}
int main() {
    Gpu g; if (!g.Init()) { std::puts("SKIP no D3D12 device"); return 77; }
    media::render::NeuralUpscalePass trained, random, stress, analytic;
    CHECK(SUCCEEDED(trained.Create(g.device, g.queue, media::render::kNeuralWeights, 1920, 1080)));
    const NeuralModel random_model = RandomModel(0.3f), stress_model = RandomModel(20.0f);
    CHECK(SUCCEEDED(random.Create(g.device, g.queue, random_model, 48, 40)) && SUCCEEDED(stress.Create(g.device, g.queue, stress_model, 48, 40)));
    // Analytic model: f1[0] = lrelu(Y) = Y, f2[0] = lrelu(f1[0]) = Y (centre taps 1), residual r[s] = 0.5 f2[0] - 0.1 (s even),
    // -0.25 f2[0] (s odd). Output = bilinear sub-pixel + r, clamped to the range of the four samples it interpolates.
    NeuralModel hand{}; hand.conv1_weight[0][4] = 1; hand.conv2_weight[0][0][4] = 1;
    for (int s = 0; s < 4; ++s) { hand.conv3_weight[s][0] = s % 2 ? -0.25f : 0.5f; hand.conv3_bias[s] = s % 2 ? 0.0f : -0.1f; }
    CHECK(SUCCEEDED(analytic.Create(g.device, g.queue, hand, 48, 40)));
    if (failures) return 1;
    // Synthetic plane 48 x 40: a soft diagonal edge, a thin line, text-like bars and noise; limited-range 8-bit codes.
    constexpr unsigned W = 48, H = 40;
    std::vector<unsigned> codes(W * H);
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
        float v = (x + 0.6f * y) > 30 ? 200.0f : 40.0f;
        if (y == 7 || x == 33) v = 120; if ((x / 3) % 2 && y > 28 && y < 34) v = 225;
        codes[y * W + x] = static_cast<unsigned>(std::clamp(v + (Random() - 0.5f) * 12, 16.0f, 235.0f));
    }
    std::vector<float> out; unsigned ow = 0, oh = 0;
    std::puts("GPU against the CPU reference:");
    for (bool ten_bit : {false, true}) {
        Frame f; CHECK(MakeFrame(g, f, W, H, ten_bit, codes));
        // Crop [3, 40) x [2, 31): 37 x 29 (partial 16x16 tiles), offset origin.
        CHECK(Run(g, trained, 3, 2, 40, 31, false, out, ow, oh, ten_bit));
        Compare(ten_bit ? "trained weights, P010, R32F, crop 37x29 @ (3,2)" : "trained weights, NV12, R32F, crop 37x29 @ (3,2)", out, ow, oh, codes, W, 3, 2, 40, 31, ten_bit, media::render::kNeuralWeights, 2e-5f);
        CHECK(Run(g, trained, 0, 0, W, H, true, out, ow, oh, ten_bit));
        Compare(ten_bit ? "trained weights, P010, R16F, full 48x40" : "trained weights, NV12, R16F, full 48x40", out, ow, oh, codes, W, 0, 0, W, H, ten_bit, media::render::kNeuralWeights, 5e-4f);
        CHECK(Run(g, random, 5, 9, 45, 40, false, out, ow, oh, ten_bit));
        Compare("random weights (+-0.3), R32F, crop 40x31 @ (5,9)", out, ow, oh, codes, W, 5, 9, 45, 40, ten_bit, random_model, 2e-5f);
        CHECK(Run(g, analytic, 0, 0, W, H, false, out, ow, oh, ten_bit));
        Compare("analytic model, R32F (vs reference)", out, ow, oh, codes, W, 0, 0, W, H, ten_bit, hand, 2e-6f);
        // Independent closed form for the analytic model, without the reference implementation.
        double worst = 0;
        auto Y = [&](int x, int y) { const unsigned c = codes[std::clamp(y, 0, int(H) - 1) * W + std::clamp(x, 0, int(W) - 1)]; return ten_bit ? static_cast<float>((c * 4) << 6) / 65535.0f : c / 255.0f; };
        for (int y = 0; y < int(H); ++y) for (int x = 0; x < int(W); ++x) for (int s = 0; s < 4; ++s) {
            const int sx = s & 1, sy = s >> 1, nx = sx ? x + 1 : x - 1, ny = sy ? y + 1 : y - 1;
            const float base = 0.5625f * Y(x, y) + 0.1875f * Y(nx, y) + 0.1875f * Y(x, ny) + 0.0625f * Y(nx, ny);
            const float lo = std::min(std::min(Y(x, y), Y(nx, y)), std::min(Y(x, ny), Y(nx, ny)));
            const float hi = std::max(std::max(Y(x, y), Y(nx, y)), std::max(Y(x, ny), Y(nx, ny)));
            const float expected = std::clamp(base + (s % 2 ? -0.25f * Y(x, y) : 0.5f * Y(x, y) - 0.1f), lo, hi); // Deringing clamp.
            worst = (std::max)(worst, static_cast<double>(std::fabs(out[std::size_t(2 * y + sy) * ow + 2 * x + sx] - expected)));
        }
        std::printf("  %-44s max |GPU - closed form| %.2e\n", "analytic model", worst);
        CHECK(worst < 2e-6);
    }
    // Extreme input and adversarial weights: every output finite and in [0, 1].
    std::puts("Extreme input:");
    for (unsigned pattern = 0; pattern < 4; ++pattern) {
        std::vector<unsigned> extreme(W * H);
        for (unsigned i = 0; i < W * H; ++i) extreme[i] = pattern == 0 ? 0 : pattern == 1 ? 255 : pattern == 2 ? ((i % W + i / W) % 2 ? 255 : 0) : static_cast<unsigned>(Random() * 256) % 256;
        Frame f; CHECK(MakeFrame(g, f, W, H, false, extreme));
        const char* names[4]{"all 0", "all 255", "checkerboard 0/255", "uniform noise"};
        for (auto* pass : {&trained, &stress}) {
            CHECK(Run(g, *pass, 0, 0, W, H, true, out, ow, oh));
            unsigned invalid = 0; for (unsigned y = 0; y < 2 * H; ++y) for (unsigned x = 0; x < 2 * W; ++x) { const float v = out[std::size_t{y} * ow + x]; invalid += !std::isfinite(v) || v < 0 || v > 1; }
            std::printf("  %-20s %-20s invalid outputs %u\n", names[pattern], pass == &trained ? "trained weights" : "weights x20 random", invalid);
            CHECK(invalid == 0);
        }
    }
    // Flat input: the trained network adds no pattern (residual within one 8-bit code) at any level.
    {
        double worst = 0;
        for (unsigned code = 16; code <= 235; code += 7) {
            std::vector<float> flat(64, code / 255.0f), up(256);
            media::video::NeuralUpscaleReference({flat.data(), 8, 8, 8}, media::render::kNeuralWeights, up.data(), 16);
            for (float v : up) worst = (std::max)(worst, std::fabs(static_cast<double>(v) - code / 255.0));
        }
        std::printf("Flat input: largest deviation %.3f codes\n", worst * 255);
        CHECK(worst * 255 < 1.0);
    }
    // Temporal tile reuse, bit for bit: every Record of `reuse` (tiles skipped when their neighbourhood is unchanged) against
    // `full` (the same pass, reuse off), each into its own persistent output, over sequences with what reuse must notice.
    std::puts("Temporal tile reuse against full evaluation (bitwise):");
    {
        constexpr unsigned SW = 160, SH = 96;
        media::render::NeuralUpscalePass reuse, full;
        CHECK(SUCCEEDED(reuse.Create(g.device, g.queue, media::render::kNeuralWeights, SW, SH)) && SUCCEEDED(full.Create(g.device, g.queue, media::render::kNeuralWeights, SW, SH)));
        full.SetReuse(media::render::NeuralUpscalePass::TemporalReuse::Off);
        struct Crop { unsigned l, t, r, b; };
        const Crop odd{3, 2, 150, 95}, whole{0, 0, SW, SH};
        struct Sequence { const char* name; unsigned kind; bool ten_bit, half; };
        const Sequence sequences[]{{"static, NV12, R32F", 0, false, false}, {"moving square + 1-code flicker, NV12, R32F", 1, false, false},
                                   {"global pan, NV12, R16F", 2, false, true}, {"cuts A/B/A, P010, R32F", 3, true, false},
                                   {"moving square + 1-code flicker, P010, R16F", 1, true, true}, {"crop and format changes", 4, false, false}};
        for (const auto& sequence : sequences) {
            Output a, b; CHECK(a.Init(g, 2 * SW, 2 * SH, sequence.half, 1) && b.Init(g, 2 * SW, 2 * SH, sequence.half, 2));
            reuse.Invalidate(); full.Invalidate();
            const unsigned before = reuse.ReuseRecords();
            unsigned mismatches = 0, compared = 0;
            for (unsigned t = 0; t < 6; ++t) {
                // Sequence 4: frames 0-2 on the odd crop (NV12), 3-4 on the whole plane, 5 the same plane as P010.
                const unsigned kind = sequence.kind == 4 ? 1 : sequence.kind;
                const Crop c = sequence.kind == 4 ? (t < 3 ? odd : whole) : (sequence.kind % 2 ? odd : whole);
                const bool ten_bit = sequence.kind == 4 ? t == 5 : sequence.ten_bit;
                Frame f; CHECK(MakeFrame(g, f, SW, SH, ten_bit, Scene(SW, SH, kind, sequence.kind == 4 ? (t < 5 ? t : 4) : t)));
                CHECK(Step(g, reuse, a, 0, c.l, c.t, c.r, c.b, ten_bit) && Step(g, full, b, 0, c.l, c.t, c.r, c.b, ten_bit));
                std::vector<std::uint32_t> ra, rb; CHECK(a.Read(g, ra) && b.Read(g, rb));
                if (ra.size() != rb.size()) continue;
                for (unsigned y = 0; y < 2 * (c.b - c.t); ++y) for (unsigned x = 0; x < 2 * (c.r - c.l); ++x, ++compared)
                    mismatches += ra[std::size_t{y} * a.w + x] != rb[std::size_t{y} * b.w + x];
            }
            const unsigned reused = reuse.ReuseRecords() - before;
            std::printf("  %-44s %u of %u outputs differ, %u of 6 Records reused tiles\n", sequence.name, mismatches, compared, reused);
            CHECK(mismatches == 0 && compared > 0);
            // Each change of crop or format starts over: 0 (first), 3 (crop), 5 (format) evaluate everything.
            CHECK(reused == (sequence.kind == 4 ? 3u : 5u));
        }
        // Exactly the tiles whose clamped 20x20 neighbourhood changed are evaluated. The output is poisoned between two
        // Records (breaking the contract on purpose): a skipped tile keeps the sentinel, an evaluated one is overwritten.
        std::puts("Temporal tile reuse: which tiles are evaluated:");
        const Crop c = odd; const unsigned cw = c.r - c.l, ch = c.b - c.t, tiles_x = (cw + 15) / 16, tiles_y = (ch + 15) / 16;
        Output a, b; CHECK(a.Init(g, 2 * SW, 2 * SH, false, 1) && b.Init(g, 2 * SW, 2 * SH, false, 2));
        std::uint32_t sentinel = 0; std::memcpy(&sentinel, &kSentinel, 4);
        struct Change { int x, y; const char* name; }; // Crop-relative texel changed by one code; x < 0: none.
        const Change changes[]{{-1, -1, "nothing changed"}, {33, 47, "one texel on a 2x2 tile corner"}, {37, 47, "one texel inside"},
                               {0, int(ch) - 1, "one texel on the crop's corner"}, {int(cw) - 1, 0, "one texel on the opposite corner"}};
        for (const auto& change : changes) {
            const auto base = Scene(SW, SH, 0, 0); auto moved = base;
            if (change.x >= 0) moved[std::size_t(c.t + change.y) * SW + c.l + change.x] += 1;
            Frame f0, f1; CHECK(MakeFrame(g, f0, SW, SH, false, base, 0) && MakeFrame(g, f1, SW, SH, false, moved, 3));
            reuse.Invalidate(); full.Invalidate();
            CHECK(Step(g, reuse, a, 0, c.l, c.t, c.r, c.b, false) && a.Fill(g, sentinel) && Step(g, reuse, a, 3, c.l, c.t, c.r, c.b, false));
            CHECK(Step(g, full, b, 3, c.l, c.t, c.r, c.b, false));
            std::vector<std::uint32_t> ra, rb; CHECK(a.Read(g, ra) && b.Read(g, rb));
            if (ra.size() != rb.size()) continue;
            unsigned evaluated = 0, expected = 0, wrong = 0;
            for (unsigned ty = 0; ty < tiles_y; ++ty) for (unsigned tx = 0; tx < tiles_x; ++tx) {
                // The tile's neighbourhood: [16 t - 2, 16 t + 18) per axis, clamped to the crop.
                const auto covers = [](int p, unsigned t, unsigned size) { return p >= std::max(0, int(16 * t) - 2) && p <= std::min(int(size) - 1, int(16 * t) + 17); };
                const bool due = change.x >= 0 && covers(change.x, tx, cw) && covers(change.y, ty, ch);
                bool kept = true, exact = true;
                for (unsigned y = 32 * ty; y < std::min(32 * ty + 32, 2 * ch); ++y) for (unsigned x = 32 * tx; x < std::min(32 * tx + 32, 2 * cw); ++x) {
                    const auto v = ra[std::size_t{y} * a.w + x];
                    kept = kept && v == sentinel; exact = exact && v == rb[std::size_t{y} * b.w + x];
                }
                evaluated += !kept; expected += due;
                wrong += due ? !exact : !kept; // Evaluated tiles equal full evaluation; the others were not touched.
            }
            std::printf("  %-36s %u of %u tiles evaluated (expected %u), %u wrong\n", change.name, evaluated, tiles_x * tiles_y, expected, wrong);
            CHECK(evaluated == expected && wrong == 0);
        }
        // Invalidate: everything is evaluated again.
        Frame f; CHECK(MakeFrame(g, f, SW, SH, false, Scene(SW, SH, 0, 0)));
        CHECK(Step(g, reuse, a, 0, c.l, c.t, c.r, c.b, false) && a.Fill(g, sentinel));
        reuse.Invalidate(); CHECK(Step(g, reuse, a, 0, c.l, c.t, c.r, c.b, false));
        std::vector<std::uint32_t> ra; CHECK(a.Read(g, ra));
        unsigned left = 0; for (unsigned y = 0; y < 2 * ch && !ra.empty(); ++y) for (unsigned x = 0; x < 2 * cw; ++x) left += ra[std::size_t{y} * a.w + x] == sentinel;
        std::printf("  %-36s %u sentinel outputs left\n", "after Invalidate", left);
        CHECK(left == 0);

        // Tolerant matching (what playback uses), against a full evaluation of every picture. Every tile must show the full
        // evaluation of some picture of the sequence (its anchor) whose 20x20 neighbourhood is within the tolerance of the
        // picture now: no code more than one step away, at most 100 steps in all. A tile showing an older evaluation than
        // that is drift: differences that added up.
        std::puts("Temporal tile reuse, tolerant matching:");
        using Reuse = media::render::NeuralUpscalePass::TemporalReuse;
        reuse.SetReuse(Reuse::Tolerant);
        // 0: 2 % of the texels one code up or down at random (compression noise on a still picture); 1: one texel of every
        // tile one code brighter with each picture (a slow drift); 2: the whole picture one code brighter with each picture.
        const auto changed = [&](unsigned kind, unsigned t) {
            auto codes = Scene(SW, SH, 0, 0);
            std::uint32_t s = 977u * (t + 1);
            for (unsigned y = 0; y < SH; ++y) for (unsigned x = 0; x < SW; ++x) {
                auto& v = codes[std::size_t{y} * SW + x];
                if (kind == 0 && t) { s = s * 1664525u + 1013904223u; if ((s >> 8) % 50 == 0) v = (s >> 20) & 1 ? v + 1 : v - 1; }
                if (kind == 1 && x % 16 == 7 && y % 16 == 7) v += t;
                if (kind == 2) v += t;
            }
            return codes;
        };
        const char* names[]{"2 % of texels +-1 code", "one texel a tile +1 code a picture", "whole picture +1 code a picture"};
        for (unsigned kind = 0; kind < 3; ++kind) {
            Output ta, tb; CHECK(ta.Init(g, 2 * SW, 2 * SH, false, 1) && tb.Init(g, 2 * SW, 2 * SH, false, 2));
            reuse.Invalidate(); full.Invalidate();
            std::vector<std::vector<std::uint32_t>> evaluations; std::vector<std::vector<unsigned>> pictures;
            unsigned drift = 0, unmatched = 0, reused = 0, evaluated = 0; double worst = 0, sum = 0; std::size_t count = 0;
            for (unsigned t = 0; t < 9; ++t) {
                pictures.push_back(changed(kind, t));
                Frame picture; CHECK(MakeFrame(g, picture, SW, SH, false, pictures.back()));
                const auto before = reuse.TileStatistics();
                CHECK(Step(g, reuse, ta, 0, 0, 0, SW, SH, false) && Step(g, full, tb, 0, 0, 0, SW, SH, false));
                const auto after = reuse.TileStatistics();
                if (t) { reused += after.reused - before.reused; evaluated += after.evaluated - before.evaluated; }
                std::vector<std::uint32_t> got, want; CHECK(ta.Read(g, got) && tb.Read(g, want));
                if (got.size() != want.size()) break;
                evaluations.push_back(want);
                for (unsigned ty = 0; ty < SH / 16; ++ty) for (unsigned tx = 0; tx < SW / 16; ++tx) {
                    // The most recent picture whose full evaluation this tile shows, bit for bit.
                    int shown = -1;
                    for (int m = static_cast<int>(t); m >= 0 && shown < 0; --m) {
                        bool same = true;
                        for (unsigned y = 32 * ty; y < 32 * ty + 32 && same; ++y) for (unsigned x = 32 * tx; x < 32 * tx + 32 && same; ++x)
                            same = got[std::size_t{y} * ta.w + x] == evaluations[static_cast<unsigned>(m)][std::size_t{y} * ta.w + x];
                        if (same) shown = m;
                    }
                    if (shown < 0) { ++unmatched; continue; }
                    unsigned most = 0, steps = 0; // That picture's neighbourhood against this one's (clamped to the plane).
                    for (int y = int(16 * ty) - 2; y < int(16 * ty) + 18; ++y) for (int x = int(16 * tx) - 2; x < int(16 * tx) + 18; ++x) {
                        const auto at = std::size_t(std::clamp(y, 0, int(SH) - 1)) * SW + std::size_t(std::clamp(x, 0, int(SW) - 1));
                        const int then = int(pictures[static_cast<unsigned>(shown)][at]), now = int(pictures[t][at]);
                        const unsigned dd = static_cast<unsigned>(then > now ? then - now : now - then); most = (std::max)(most, dd); steps += dd;
                    }
                    drift += most > 1 || steps > 100;
                    for (unsigned y = 32 * ty; y < 32 * ty + 32; ++y) for (unsigned x = 32 * tx; x < 32 * tx + 32; ++x) {
                        const auto at = std::size_t{y} * ta.w + x;
                        float va = 0, vb = 0; std::memcpy(&va, &got[at], 4); std::memcpy(&vb, &want[at], 4);
                        const double dv = static_cast<double>(std::fabs(va - vb)) * 255.0; worst = (std::max)(worst, dv); sum += dv; ++count;
                    }
                }
            }
            std::printf("  %-36s tiles reused %u, evaluated %u (pictures 1-8); showing an evaluation beyond the tolerance %u, none %u; "
                        "difference from a full evaluation of the picture: largest %.3f, mean %.4f codes\n",
                        names[kind], reused, evaluated, drift, unmatched, worst, count ? sum / static_cast<double>(count) : 0.0);
            CHECK(drift == 0 && unmatched == 0);
            if (kind == 0) CHECK(reused > 9 * evaluated); // Noise below the tolerance: nearly everything reused.
            if (kind == 1) CHECK(reused > 0 && evaluated >= reused); // Every other picture: the drift reaches two codes.
            if (kind == 2) CHECK(reused == 0 && worst == 0.0);       // A mean difference of one code: never reused.
        }
        reuse.SetReuse(Reuse::Exact);
    }
    // Zero allocations while recording and executing (the production path).
    {
        Frame f; CHECK(MakeFrame(g, f, W, H, false, codes));
        const auto desc = Texture(2 * W, 2 * H, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ID3D12Resource* output = Resource(g.device, D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = DXGI_FORMAT_R16_FLOAT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.device->CreateUnorderedAccessView(output, nullptr, &uav, g.Cpu(1));
        allocation_probe::Begin();
        for (unsigned i = 0; i < 20; ++i) {
            ID3D12DescriptorHeap* heaps[]{g.heap}; g.list->SetDescriptorHeaps(1, heaps);
            trained.Record(g.list, g.Gpu_(0), g.Gpu_(1), 0, 0, W, H);
            if (!g.Submit()) { CHECK(false); break; }
        }
        const auto allocations = allocation_probe::End(); std::printf("Recording: %llu allocations\n", static_cast<unsigned long long>(allocations));
        CHECK(allocations == 0);
        Release(output);
    }
    // GPU time of the pass alone (timestamps, best of 12): 720p and 1080p sources into R16F.
    {
        UINT64 frequency = 0; CHECK(SUCCEEDED(g.queue->GetTimestampFrequency(&frequency)));
        g.timestamps = Resource(g.device, D3D12_HEAP_TYPE_READBACK, Buffer(16), D3D12_RESOURCE_STATE_COPY_DEST);
        struct Case { unsigned w, h, bar; const char* name; };
        // Detail everywhere (worst case), and a 2.39:1 picture letterboxed in 1080p (flat bars of 140 rows).
        for (const Case& size : {Case{1280, 720, 0, "detail"}, Case{1920, 1080, 0, "detail"}, Case{1920, 1080, 140, "letterboxed 2.39:1"}}) {
            const unsigned w = size.w, h = size.h;
            std::vector<unsigned> big(std::size_t{w} * h);
            for (unsigned i = 0; i < w * h; ++i) big[i] = i / w < size.bar || i / w >= h - size.bar ? 16 : 16 + static_cast<unsigned>(Random() * 219);
            Frame f; CHECK(MakeFrame(g, f, w, h, false, big));
            const auto desc = Texture(2 * w, 2 * h, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            ID3D12Resource* output = Resource(g.device, D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = DXGI_FORMAT_R16_FLOAT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g.device->CreateUnorderedAccessView(output, nullptr, &uav, g.Cpu(1));
            double best = 1e9;
            { // Warm-up: an integrated GPU idles at a low clock and ramps only under sustained load.
                ID3D12DescriptorHeap* heaps[]{g.heap}; g.list->SetDescriptorHeaps(1, heaps);
                for (unsigned r = 0; r < 8; ++r) { trained.Invalidate(); trained.Record(g.list, g.Gpu_(0), g.Gpu_(1), 0, 0, w, h); }
                CHECK(g.Submit());
            }
            // One timed Record from the luma SRV at `luma`, best of 12; `invalidate`: a full evaluation each time; `alternate`:
            // the SRVs at 3 and 0 in turn, starting with 3 after a Record of 0 (each Record against the other picture).
            const auto timed = [&](bool invalidate, bool alternate) {
                double fastest = 1e9;
                for (unsigned run = 0; run < 12; ++run) {
                    if (invalidate) trained.Invalidate();
                    ID3D12DescriptorHeap* heaps[]{g.heap}; g.list->SetDescriptorHeaps(1, heaps);
                    g.list->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
                    trained.Record(g.list, g.Gpu_(alternate && run % 2 == 0 ? 3 : 0), g.Gpu_(1), 0, 0, w, h);
                    g.list->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
                    g.list->ResolveQueryData(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, g.timestamps, 0);
                    if (!g.Submit()) { CHECK(false); break; }
                    UINT64* t = nullptr; const D3D12_RANGE range{0, 16};
                    if (SUCCEEDED(g.timestamps->Map(0, &range, reinterpret_cast<void**>(&t)))) {
                        fastest = (std::min)(fastest, static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(frequency)); g.timestamps->Unmap(0, nullptr);
                    }
                }
                return fastest;
            };
            best = timed(true, false);
            const double gmac = static_cast<double>(w) * h * 1328 / 1e9;
            std::printf("Timing %ux%u -> %ux%u (%s): %.2f ms (%.2f GMAC full-frame equivalent, %.0f GMAC/s)\n", w, h, 2 * w, 2 * h, size.name, best, gmac, gmac / (best / 1000));
            // Temporal tile reuse: the same picture again (static shot), and two pictures that differ in a centred quarter of
            // the area (a talking head on a still background), alternated.
            trained.Invalidate(); { ID3D12DescriptorHeap* heaps[]{g.heap}; g.list->SetDescriptorHeaps(1, heaps); trained.Record(g.list, g.Gpu_(0), g.Gpu_(1), 0, 0, w, h); CHECK(g.Submit()); }
            const double still = timed(false, false);
            auto other = big;
            for (unsigned y = h / 4; y < 3 * h / 4; ++y) for (unsigned x = w / 4; x < 3 * w / 4; ++x) other[std::size_t{y} * w + x] = 16 + static_cast<unsigned>(Random() * 219);
            Frame moving; CHECK(MakeFrame(g, moving, w, h, false, other, 3));
            const double talking = timed(false, true);
            std::printf("  temporal reuse: static %.2f ms (%.1fx), centred quarter moving %.2f ms (%.1fx)\n", still, best / still, talking, best / talking);
            CHECK(still * 2 < best && talking < best && still <= talking);
            // Compression noise on a still picture: 2 % of the texels one code up or down, alternated with the clean picture.
            // Exact matching evaluates nearly every tile; tolerant matching keeps them.
            auto grain = big; std::uint32_t s = 31;
            for (auto& v : grain) { s = s * 1664525u + 1013904223u; if ((s >> 8) % 50 == 0) v = (s >> 20) & 1 ? v + 1 : v - 1; }
            Frame noisy; CHECK(MakeFrame(g, noisy, w, h, false, grain, 3));
            const double exact_noise = timed(false, true);
            trained.SetReuse(media::render::NeuralUpscalePass::TemporalReuse::Tolerant);
            const double tolerant_noise = timed(false, true);
            trained.SetReuse(media::render::NeuralUpscalePass::TemporalReuse::Exact);
            std::printf("  still picture with +-1 code noise on 2 %% of texels: exact %.2f ms (%.1fx), tolerant %.2f ms (%.1fx)\n", exact_noise,
                        best / exact_noise, tolerant_noise, best / tolerant_noise);
            CHECK(tolerant_noise * 2 < exact_noise);
            Release(output);
        }
    }
    std::printf("Neural upscale GPU: %d failures\n", failures);
    return failures ? 1 : 0;
}
