// Production colour pipeline (render::ColorPipeline) against an independent double-precision CPU reference.
// A synthetic P010 surface is uploaded (test input, not decoded video); results come back only as scalars:
// a sum and a maximum of per-channel differences, and the three words of the GPU peak tracker.
#include "media/render/ColorPipeline.hpp"
#include "ColorCompareShader.hpp"
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
template <class T> void ReleaseCom(T*& p) { if (p) { p->Release(); p = nullptr; } }
constexpr unsigned W = 64, H = 32, CW = W / 2, CH = H / 2;

// ---- Test image (10-bit limited range codes) ----
unsigned LumaCode(unsigned x) { return 64 + (876 * x + (W - 1) / 2) / (W - 1); }
constexpr int kBands[8][2]{{0, 0}, {200, -100}, {-150, 220}, {300, 50}, {-250, -250}, {100, 300}, {-300, 100}, {50, -350}};
unsigned ChromaCode(unsigned row, unsigned component) { return static_cast<unsigned>(512 + kBands[(row / 2) % 8][component]); }
double Unorm(unsigned code) { return code * 64.0 / 65535.0; } // P010 stores the code in the high bits.

// ---- Reference maths (mirrors HdrToneMap.hlsl and Nv12ToRgb.hlsl in double precision) ----
constexpr double M1 = 0.1593017578125, M2 = 78.84375, C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;
double PqToNits(double e) { const double p = std::pow(std::max(e, 0.0), 1 / M2); return 10000 * std::pow(std::max((p - C1) / (C2 - C3 * p), 0.0), 1 / M1); }
double NitsToPq(double n) { const double p = std::pow(std::max(n, 0.0) / 10000, M1); return std::pow(std::max((C1 + C2 * p) / (1 + C3 * p), 0.0), M2); }
double HlgToScene(double e) { const double a = 0.17883277, b = 0.28466892, c = 0.55991073; return e <= 0.5 ? e * e / 3 : (std::exp((e - c) / a) + b) / 12; }
double Eetf(double pq, double source, double target) {
    const double e1 = pq / source, max_lum = target / source;
    if (max_lum >= 1) return pq;
    const double ks = 1.5 * max_lum - 0.5;
    if (e1 < ks) return e1 * source;
    const double t = (e1 - ks) / (1 - ks), t2 = t * t, t3 = t2 * t;
    const double e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * max_lum;
    return std::min(e2, max_lum) * source;
}
double Saturate(double v) { return std::clamp(v, 0.0, 1.0); }
struct Case { const char* name; unsigned transfer, primaries; double peak_nits; };
struct Rgb { double r, g, b; };
// Bilinear chroma exactly as sampled at output pixel (x, y) for a 1:1 conversion (texel-centre luma).
void Chroma(unsigned x, unsigned y, double& cb, double& cr) {
    const double fx = x / 2.0 - 0.25, fy = y / 2.0 - 0.25;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const double ax = fx - x0, ay = fy - y0;
    auto at = [](int row, unsigned c) { return Unorm(ChromaCode(static_cast<unsigned>(std::clamp(row, 0, static_cast<int>(CH) - 1)), c)); };
    for (unsigned c = 0; c < 2; ++c) {
        const double v = (1 - ay) * at(y0, c) + ay * at(y0 + 1, c); // Constant along a row: x weights cancel.
        (c ? cr : cb) = v;
    }
    (void)ax;
}
Rgb DecodeRgb(unsigned x, unsigned y, bool bt2020) {
    const double code = 64.0 / 65535.0, depth = 4.0;
    double luma = Unorm(LumaCode(x)), cb = 0, cr = 0; Chroma(x, y, cb, cr);
    cb -= 128 * depth * code; cr -= 128 * depth * code;
    luma = (luma - 16 * depth * code) / (219 * depth * code); cb /= 224 * depth * code; cr /= 224 * depth * code;
    const double kr = bt2020 ? 0.2627 : 0.299, kb = bt2020 ? 0.0593 : 0.114;
    const double red = luma + 2 * (1 - kr) * cr, blue = luma + 2 * (1 - kb) * cb;
    const double green = (luma - kr * red - kb * blue) / (1 - kr - kb);
    return {Saturate(red), Saturate(green), Saturate(blue)};
}
Rgb ToBt709(Rgb l) {
    return {1.660491 * l.r - 0.587641 * l.g - 0.072850 * l.b, -0.124550 * l.r + 1.132900 * l.g - 0.008349 * l.b, -0.018151 * l.r - 0.100579 * l.g + 1.118730 * l.b};
}
// HDR light in cd/m2 before tone mapping (used for the peak detector expectation too).
Rgb HdrNits(const Rgb& e, unsigned transfer) {
    if (transfer == 18) {
        const Rgb scene{HlgToScene(e.r), HlgToScene(e.g), HlgToScene(e.b)};
        const double ys = 0.2627 * scene.r + 0.6780 * scene.g + 0.0593 * scene.b;
        const double gain = 1000 * std::pow(std::max(ys, 1e-6), 0.2);
        return {gain * scene.r, gain * scene.g, gain * scene.b};
    }
    return {PqToNits(e.r), PqToNits(e.g), PqToNits(e.b)};
}
Rgb Reference(unsigned x, unsigned y, const Case& c) {
    const auto e = DecodeRgb(x, y, true);
    if (c.transfer == 16 || c.transfer == 18) {
        const auto nits = HdrNits(e, c.transfer);
        const double brightest = std::max({nits.r, nits.g, nits.b});
        const double target = 203.0;
        const double mapped = PqToNits(Eetf(NitsToPq(brightest), std::max(NitsToPq(c.peak_nits), 1e-4), NitsToPq(target)));
        Rgb light = brightest > 0 ? Rgb{nits.r * mapped / brightest / target, nits.g * mapped / brightest / target, nits.b * mapped / brightest / target} : Rgb{0, 0, 0};
        light = ToBt709(light);
        const double lum = 0.2126 * light.r + 0.7152 * light.g + 0.0722 * light.b, lowest = std::min({light.r, light.g, light.b});
        if (lowest < 0 && lum > 0) {
            const double k = lum / (lum - lowest);
            light = {lum + (light.r - lum) * k, lum + (light.g - lum) * k, lum + (light.b - lum) * k};
        }
        return {std::pow(Saturate(light.r), 1 / 2.4), std::pow(Saturate(light.g), 1 / 2.4), std::pow(Saturate(light.b), 1 / 2.4)};
    }
    // SDR with BT.2020 primaries: display-light gamut conversion (Nv12ToRgb.hlsl).
    Rgb light{std::pow(e.r, 2.4), std::pow(e.g, 2.4), std::pow(e.b, 2.4)};
    light = ToBt709(light);
    return {std::pow(Saturate(light.r), 1 / 2.4), std::pow(Saturate(light.g), 1 / 2.4), std::pow(Saturate(light.b), 1 / 2.4)};
}
std::uint8_t Quantize(double v) { return static_cast<std::uint8_t>(std::floor(Saturate(v) * 255 + 0.5)); }

// ---- Properties of the reference curve itself ----
void CurveProperties() {
    const Case pq{"PQ", 16, 9, 1000};
    double previous = -1;
    bool monotonic = true, neutral = true;
    for (unsigned x = 0; x < W; ++x) {
        const auto rgb = Reference(x, 0, pq); // Band 0: neutral chroma.
        monotonic = monotonic && rgb.g >= previous - 1e-12; previous = rgb.g;
        neutral = neutral && std::abs(rgb.r - rgb.g) < 1e-5 && std::abs(rgb.b - rgb.g) < 1e-5;
    }
    CHECK(monotonic); CHECK(neutral);
    // Below the BT.2390 knee the EETF is the identity: 50 cd/m2 stays 50/203 of SDR white.
    const double knee = (1.5 * NitsToPq(203) / NitsToPq(1000) - 0.5) * NitsToPq(1000);
    CHECK(PqToNits(knee) > 80 && std::abs(PqToNits(Eetf(NitsToPq(50), NitsToPq(1000), NitsToPq(203))) - 50) < 1e-6);
    // Above it, the roll-off compresses monotonically and never exceeds the target.
    CHECK(PqToNits(Eetf(NitsToPq(1000), NitsToPq(1000), NitsToPq(203))) <= 203.0 + 1e-6);
    CHECK(PqToNits(Eetf(NitsToPq(600), NitsToPq(1000), NitsToPq(203))) < PqToNits(Eetf(NitsToPq(900), NitsToPq(1000), NitsToPq(203))));
}

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
ID3D12Resource* Texture(ID3D12Device* device, DXGI_FORMAT format, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = W; desc.Height = H;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = flags;
    ID3D12Resource* resource = nullptr;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource))) ? resource : nullptr;
}
// Copies CPU bytes into every subresource of `texture` through one upload buffer (test setup only).
bool Upload(Gpu& gpu, ID3D12Resource* texture, unsigned subresources, const std::vector<std::vector<std::uint8_t>>& planes,
            const std::vector<unsigned>& row_bytes, ID3D12Resource*& staging) {
    const auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2]{}; UINT rows[2]{}; UINT64 sizes[2]{}, total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, subresources, 0, layouts, rows, sizes, &total);
    ReleaseCom(staging); staging = Buffer(gpu.device, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!staging) return false;
    std::uint8_t* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    for (unsigned s = 0; s < subresources; ++s)
        for (UINT r = 0; r < rows[s]; ++r)
            std::memcpy(mapped + layouts[s].Offset + static_cast<UINT64>(r) * layouts[s].Footprint.RowPitch, planes[s].data() + static_cast<std::size_t>(r) * row_bytes[s], row_bytes[s]);
    staging->Unmap(0, nullptr);
    for (unsigned s = 0; s < subresources; ++s) {
        D3D12_TEXTURE_COPY_LOCATION to{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; to.SubresourceIndex = s;
        D3D12_TEXTURE_COPY_LOCATION from{staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = layouts[s];
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    return true;
}
bool ReadWords(Gpu& gpu, ID3D12Resource* source, unsigned count, std::uint32_t* words) {
    auto* readback = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return false;
    gpu.list->CopyBufferRegion(readback, 0, source, 0, count * 4ull);
    bool good = gpu.Submit();
    void* mapped = nullptr; const D3D12_RANGE range{0, count * 4ull};
    if (good && SUCCEEDED(readback->Map(0, &range, &mapped))) { std::memcpy(words, mapped, count * 4ull); const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none); }
    else good = false;
    ReleaseCom(readback); return good;
}

// ---- media_hdr_tonemap_epoch_tests: the dynamic peak per epoch, in presentation time ----
// Three PQ scenes, neutral grey ramps up to 1000 (bright), 600 (mid) and 120 cd/m2 (dark). The first picture of an epoch
// (a seek's landing) must be converted exactly as the same picture decoded cold, whatever was shown before; a scene cut
// takes the new scene's own peak; the smoothing follows presentation time, not the number of pictures; a picture that
// presents before one already folded adds no time (PeakTiming).
struct Scene { const char* name; double nits; ID3D12Resource* surface = nullptr; unsigned descriptor = 0; };
unsigned RampCode(unsigned x, double nits) { return 64 + static_cast<unsigned>(std::lround(NitsToPq(nits) * 876.0 * x / (W - 1))); }
int Epoch(Gpu& gpu) {
    HRESULT hr = S_OK;
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    hr = gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    const auto stride = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](unsigned i) { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * stride; return h; };
    auto gpu_handle = [&](unsigned i) { auto h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(i) * stride; return h; };
    Scene scenes[3]{{"bright", 1000}, {"mid", 600}, {"dark", 120}};
    std::vector<ID3D12Resource*> staging(3, nullptr);
    for (unsigned k = 0; k < 3 && SUCCEEDED(hr); ++k) {
        auto& scene = scenes[k]; scene.descriptor = 2 * k;
        scene.surface = Texture(gpu.device, DXGI_FORMAT_P010, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
        if (!scene.surface) { hr = E_FAIL; break; }
        for (unsigned plane = 0; plane < 2; ++plane) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = plane ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
            gpu.device->CreateShaderResourceView(scene.surface, &srv, cpu(scene.descriptor + plane));
        }
        std::vector<std::vector<std::uint8_t>> planes(2);
        planes[0].resize(W * H * 2); planes[1].resize(CW * CH * 4);
        for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
            const auto v = static_cast<std::uint16_t>(RampCode(x, scene.nits) << 6); std::memcpy(&planes[0][(y * W + x) * 2], &v, 2);
        }
        for (unsigned i = 0; i < CW * CH * 2; ++i) { const auto v = static_cast<std::uint16_t>(512u << 6); std::memcpy(&planes[1][i * 2], &v, 2); }
        CHECK(Upload(gpu, scene.surface, 2, planes, {W * 2, CW * 4}, staging[k]));
        auto barrier = Transition(scene.surface, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu.list->ResourceBarrier(1, &barrier);
    }
    auto* output = Texture(gpu.device, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (SUCCEEDED(hr) && output) gpu.device->CreateUnorderedAccessView(output, nullptr, nullptr, cpu(6));
    if (FAILED(hr) || !output || !gpu.Submit()) { std::printf("FAIL epoch setup: %08lx\n", static_cast<unsigned long>(hr)); return 1; }
    ID3D12DescriptorHeap* heaps[]{heap};
    // One picture of `scene` at presentation time `pts` (100 ns) through `color`, timed as the renderer times it.
    auto convert = [&](media::render::ColorPipeline& color, media::render::PeakTiming& timing, const Scene& scene, std::int64_t pts) {
        std::uint32_t flags = 0; const float dt = timing.Step(pts, flags);
        D3D12_RESOURCE_BARRIER written{}; written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; written.UAV.pResource = output;
        gpu.list->ResourceBarrier(1, &written); // The previous conversion's writes to the same picture come first.
        gpu.list->SetDescriptorHeaps(1, heaps);
        media::render::ColorConstants constants{W, H, W, H, 0, 0, W, H, 9, 0, 1, 9, 16, 0, 1000.0f, 203.0f};
        constants.peak_dt = dt; constants.peak_flags = flags;
        color.Record(gpu.list, gpu_handle(scene.descriptor), gpu_handle(6), constants);
    };
    // The converted picture, as bytes.
    auto picture = [&](std::vector<std::uint8_t>& bytes) {
        const auto desc = output->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT rows = 0; UINT64 row = 0, total = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &row, &total);
        auto* readback = Buffer(gpu.device, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        auto barrier = Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION to{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = layout;
        D3D12_TEXTURE_COPY_LOCATION from{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); gpu.list->ResourceBarrier(1, &barrier);
        bool good = readback && gpu.Submit();
        std::uint8_t* mapped = nullptr; bytes.assign(W * H * 4, 0);
        if (good && SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
            for (unsigned y = 0; y < H; ++y) std::memcpy(&bytes[y * W * 4], mapped + layout.Offset + static_cast<UINT64>(y) * layout.Footprint.RowPitch, W * 4);
            const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
        } else good = false;
        ReleaseCom(readback); CHECK(good);
    };
    auto state = [&](media::render::ColorPipeline& color, float& smoothed, std::uint32_t& cuts, std::uint32_t& starts) {
        std::uint32_t words[5]{}; CHECK(ReadWords(gpu, color.PeakState(), 5, words));
        std::memcpy(&smoothed, &words[1], 4); cuts = words[3]; starts = words[4];
    };
    auto differ = [](const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
        unsigned worst = 0; for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, static_cast<unsigned>(std::abs(a[i] - b[i]))); return worst;
    };
    constexpr std::int64_t k24 = 10000000 / 24;
    // Cold references: each scene as the first picture of a fresh pipeline.
    std::vector<std::uint8_t> cold[3];
    float cold_peak[3]{};
    for (unsigned k = 0; k < 3; ++k) {
        media::render::ColorPipeline color; CHECK(SUCCEEDED(color.Create(gpu.device))); media::render::PeakTiming timing;
        convert(color, timing, scenes[k], 50000000); picture(cold[k]);
        std::uint32_t cuts = 0, starts = 0; state(color, cold_peak[k], cuts, starts);
        CHECK(starts == 1 && cuts == 0);
        color.Release();
    }
    std::printf("Cold peaks: bright %.0f, mid %.0f, dark %.0f cd/m2 (floor 100)\n", PqToNits(cold_peak[0]), PqToNits(cold_peak[1]), PqToNits(cold_peak[2]));
    CHECK(std::abs(PqToNits(cold_peak[0]) - 1000) < 15 && std::abs(PqToNits(cold_peak[1]) - 600) < 10 && std::abs(PqToNits(cold_peak[2]) - 120) < 3);
    // A second of the bright scene, then `next`: as a seek's landing (a new epoch), or as the next picture.
    auto after_bright = [&](const Scene& next, bool seek, std::vector<std::uint8_t>& out, std::uint32_t& cuts, float& smoothed) {
        media::render::ColorPipeline color; CHECK(SUCCEEDED(color.Create(gpu.device))); media::render::PeakTiming timing;
        std::int64_t pts = 0;
        for (unsigned i = 0; i < 24; ++i, pts += k24) convert(color, timing, scenes[0], pts);
        if (seek) { timing.Restart(); pts = 50000000; }
        convert(color, timing, next, pts); picture(out);
        std::uint32_t starts = 0; state(color, smoothed, cuts, starts);
        color.Release();
    };
    std::vector<std::uint8_t> out; std::uint32_t cuts = 0; float smoothed = 0;
    // 1. Seek from the bright scene to the dark one and to the mid one: the landing equals the cold picture exactly.
    after_bright(scenes[2], true, out, cuts, smoothed);
    std::printf("Seek bright -> dark: landing differs from cold by %u (8-bit steps); peak %.0f cd/m2\n", differ(out, cold[2]), PqToNits(smoothed));
    CHECK(out == cold[2] && smoothed == cold_peak[2]);
    after_bright(scenes[1], true, out, cuts, smoothed);
    std::printf("Seek bright -> mid: landing differs from cold by %u\n", differ(out, cold[1]));
    CHECK(out == cold[1] && smoothed == cold_peak[1] && cuts == 0);
    // 2. Without the new epoch, 600 after 1000 cd/m2 is no scene cut (the peak moves by less than the threshold): the
    //    inherited exposure shows, which is what the reset removes.
    after_bright(scenes[1], false, out, cuts, smoothed);
    const auto inherited = differ(out, cold[1]);
    std::printf("Bright -> mid in one epoch (no cut): differs from cold by %u, peak %.0f cd/m2 (inherited)\n", inherited, PqToNits(smoothed));
    CHECK(inherited > 0 && cuts == 0 && smoothed > cold_peak[1]);
    // 3. A scene cut in one epoch (1000 -> 120 cd/m2): the dark scene takes its own peak at once, exactly as cold.
    after_bright(scenes[2], false, out, cuts, smoothed);
    std::printf("Bright -> dark in one epoch: scene cut %u, differs from cold by %u\n", cuts, differ(out, cold[2]));
    CHECK(cuts == 1 && out == cold[2] && smoothed == cold_peak[2]);
    // 4. Presentation time, not picture count: from mid to bright over 0.25 s at 24 and at 60 pictures a second.
    float rate_peak[2]{};
    for (unsigned r = 0; r < 2; ++r) {
        media::render::ColorPipeline color; CHECK(SUCCEEDED(color.Create(gpu.device))); media::render::PeakTiming timing;
        const unsigned fps = r ? 60 : 24, steps = r ? 15 : 6;
        convert(color, timing, scenes[1], 0);
        for (unsigned i = 1; i <= steps; ++i) convert(color, timing, scenes[0], static_cast<std::int64_t>(i) * 10000000 / fps);
        CHECK(gpu.Submit());
        std::uint32_t starts = 0; state(color, rate_peak[r], cuts, starts);
        color.Release();
    }
    const double expected = cold_peak[0] + (cold_peak[1] - cold_peak[0]) * std::exp(-0.25 / 0.1);
    std::printf("Mid -> bright for 0.25 s: peak PQ %.5f at 24 fps, %.5f at 60 fps (attack 100 ms: %.5f)\n", rate_peak[0], rate_peak[1], expected);
    CHECK(std::abs(rate_peak[0] - rate_peak[1]) < 2e-4 && std::abs(rate_peak[0] - expected) < 2e-4);
    // 5. Decode order: pictures arrive I0 P3 B1 B2 P6 B4 B5; the B-pictures present before P3/P6, already folded, so
    //    they add no time, and the smoothing sees only the presentation-time steps.
    {
        media::render::PeakTiming timing; std::uint32_t flags = 0;
        const int order[]{0, 3, 1, 2, 6, 4, 5};
        float total = 0; unsigned starts = 0;
        for (const int n : order) { total += timing.Step(n * k24, flags); starts += flags & media::render::kPeakStart ? 1u : 0u; }
        CHECK(starts == 1 && std::abs(total - 6.0f * k24 / 1e7f) < 1e-6f);
        timing.Restart(); CHECK(timing.Step(123, flags) == 0.0f && flags == media::render::kPeakStart && timing.Step(123 + k24, flags) > 0 && flags == 0);
        std::printf("Decode-order steps: %.4f s folded for 6 presentation intervals\n", static_cast<double>(total));
    }
    for (auto& scene : scenes) ReleaseCom(scene.surface);
    for (auto* buffer : staging) ReleaseCom(buffer);
    ReleaseCom(output); ReleaseCom(heap);
    std::printf("HDR tone mapping per epoch: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    CurveProperties();
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
    if (!gpu.device) { std::puts("SKIP no hardware D3D12 device"); return failures ? 1 : 77; }
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = gpu.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&gpu.queue));
    if (SUCCEEDED(hr)) hr = gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    if (SUCCEEDED(hr)) hr = gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    if (SUCCEEDED(hr)) hr = gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator, nullptr, IID_PPV_ARGS(&gpu.list));
    if (argc > 1 && std::strcmp(argv[1], "epoch") == 0) {
        const int result = FAILED(hr) ? 1 : Epoch(gpu);
        ReleaseCom(gpu.list); ReleaseCom(gpu.allocator); ReleaseCom(gpu.fence); ReleaseCom(gpu.queue); ReleaseCom(gpu.device);
        return result;
    }
    media::render::ColorPipeline color;
    if (SUCCEEDED(hr)) hr = color.Create(gpu.device);
    // Compare pass: t0 produced, t1 expected, u0 result words.
    ID3D12RootSignature* compare_root = nullptr; ID3D12PipelineState* compare = nullptr;
    {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[0].DescriptorTable = {1, &range};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[1].Descriptor = {0, 0};
        const D3D12_ROOT_SIGNATURE_DESC signature{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *blob = nullptr, *errors = nullptr;
        if (SUCCEEDED(hr)) hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (SUCCEEDED(hr)) hr = gpu.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&compare_root));
        ReleaseCom(blob); ReleaseCom(errors);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = compare_root; pso.CS = {g_ColorCompareShader, sizeof(g_ColorCompareShader)};
        if (SUCCEEDED(hr)) hr = gpu.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&compare));
    }
    auto* source = Texture(gpu.device, DXGI_FORMAT_P010, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto* output = Texture(gpu.device, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* expected = Texture(gpu.device, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto* result = Buffer(gpu.device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (SUCCEEDED(hr)) hr = gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr) || !source || !output || !expected || !result) { std::printf("FAIL setup: %08lx\n", static_cast<unsigned long>(hr)); return 1; }
    const auto stride = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](unsigned i) { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * stride; return h; };
    auto gpu_handle = [&](unsigned i) { auto h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(i) * stride; return h; };
    // [0] luma, [1] chroma, [2] output UAV, [3] output SRV, [4] expected SRV.
    for (unsigned plane = 0; plane < 2; ++plane) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = plane ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
        gpu.device->CreateShaderResourceView(source, &srv, cpu(plane));
    }
    gpu.device->CreateUnorderedAccessView(output, nullptr, nullptr, cpu(2));
    gpu.device->CreateShaderResourceView(output, nullptr, cpu(3));
    gpu.device->CreateShaderResourceView(expected, nullptr, cpu(4));
    // Upload the P010 test surface.
    std::vector<std::vector<std::uint8_t>> planes(2);
    planes[0].resize(W * H * 2); planes[1].resize(CW * CH * 4);
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
        const auto v = static_cast<std::uint16_t>(LumaCode(x) << 6); std::memcpy(&planes[0][(y * W + x) * 2], &v, 2);
    }
    for (unsigned y = 0; y < CH; ++y) for (unsigned x = 0; x < CW; ++x) for (unsigned c = 0; c < 2; ++c) {
        const auto v = static_cast<std::uint16_t>(ChromaCode(y, c) << 6); std::memcpy(&planes[1][((y * CW + x) * 2 + c) * 2], &v, 2);
    }
    ID3D12Resource *staging = nullptr, *reference_staging = nullptr;
    CHECK(Upload(gpu, source, 2, planes, {W * 2, CW * 4}, staging));
    auto to_read = Transition(source, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.list->ResourceBarrier(1, &to_read);
    CHECK(gpu.Submit());
    ID3D12DescriptorHeap* heaps[]{heap};
    auto convert = [&](const Case& c, std::uint32_t peak_mode) {
        gpu.list->SetDescriptorHeaps(1, heaps);
        const media::render::ColorConstants constants{W, H, W, H, 0, 0, W, H, 9, 0, 1, c.primaries, c.transfer, peak_mode,
                                                     static_cast<float>(c.peak_nits), 203.0f};
        color.Record(gpu.list, gpu_handle(0), gpu_handle(2), constants);
    };
    // ---- Fixed-peak cases against the reference ----
    const Case cases[]{{"PQ, 1000 cd/m2 peak", 16, 9, 1000}, {"PQ, 4000 cd/m2 peak", 16, 9, 4000},
                       {"HLG, 1000 cd/m2 display", 18, 9, 1000}, {"SDR BT.2020 primaries", 1, 9, 1000}};
    for (const auto& c : cases) {
        std::vector<std::vector<std::uint8_t>> reference(1, std::vector<std::uint8_t>(W * H * 4));
        for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
            const auto rgb = Reference(x, y, c);
            auto* pixel = &reference[0][(y * W + x) * 4];
            pixel[0] = Quantize(rgb.r); pixel[1] = Quantize(rgb.g); pixel[2] = Quantize(rgb.b); pixel[3] = 255;
        }
        CHECK(Upload(gpu, expected, 1, reference, {W * 4}, reference_staging));
        convert(c, 1);
        D3D12_RESOURCE_BARRIER barriers[]{Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                          Transition(expected, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        gpu.list->ResourceBarrier(2, barriers);
        // A fresh committed buffer is zero-initialized; it promotes from COMMON to UAV and decays after submission.
        ReleaseCom(result); result = Buffer(gpu.device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CHECK(result != nullptr); if (!result) break;
        gpu.list->SetComputeRootSignature(compare_root); gpu.list->SetPipelineState(compare);
        gpu.list->SetComputeRootDescriptorTable(0, gpu_handle(3)); gpu.list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
        gpu.list->Dispatch(W / 8, H / 8, 1);
        std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
        barriers[1] = Transition(expected, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.list->ResourceBarrier(2, barriers);
        CHECK(gpu.Submit());
        std::uint32_t words[2]{};
        CHECK(ReadWords(gpu, result, 2, words));
        const double mean = words[0] / (3.0 * W * H);
        std::printf("%-26s GPU vs reference: mean |error| %.3f, max %u (8-bit steps)\n", c.name, mean, words[1]);
        CHECK(mean <= 0.35 && words[1] <= 2);
    }
    // ---- GPU peak tracking (dynamic mode) ----
    // PQ of the brightest max(R,G,B) of the source samples (each 2x2 luma block with its chroma sample), as the detector
    // quantizes it. The first picture is converted with its own peak (no 1000 cd/m2 guess).
    double frame_peak = 0;
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
        const double code = 64.0 / 65535.0, depth = 4.0;
        const double luma = (Unorm(LumaCode(x)) - 16 * depth * code) / (219 * depth * code);
        const double cb = (Unorm(ChromaCode(y / 2, 0)) - 128 * depth * code) / (224 * depth * code);
        const double cr = (Unorm(ChromaCode(y / 2, 1)) - 128 * depth * code) / (224 * depth * code);
        const double kr = 0.2627, kb = 0.0593, red = luma + 2 * (1 - kr) * cr, blue = luma + 2 * (1 - kb) * cb;
        const double green = (luma - kr * red - kb * blue) / (1 - kr - kb);
        frame_peak = std::max(frame_peak, std::max({Saturate(red), Saturate(green), Saturate(blue)})); // PQ is monotonic.
    }
    const double quantized = std::floor(std::clamp(frame_peak, 0.0, 1.0) * 65535 + 0.5) / 65535;
    const Case dynamic{"PQ dynamic", 16, 9, 1000};
    convert(dynamic, 0); CHECK(gpu.Submit());
    std::uint32_t words[3]{}; float smoothed = 0;
    CHECK(ReadWords(gpu, color.PeakState(), 3, words)); std::memcpy(&smoothed, &words[1], 4);
    std::printf("Peak tracker after 1 frame: smoothed %.5f (reference %.5f), frames %u\n", smoothed, quantized, words[2]);
    CHECK(words[2] == 1 && words[0] == 0 && std::abs(smoothed - quantized) <= 2.0 / 65535); // Detector cleared once folded.
    for (unsigned i = 0; i < 29; ++i) convert(dynamic, 0);
    CHECK(gpu.Submit());
    CHECK(ReadWords(gpu, color.PeakState(), 3, words)); std::memcpy(&smoothed, &words[1], 4);
    std::printf("Peak tracker after 30 frames: smoothed PQ %.5f = %.0f cd/m2 (frame peak %.0f cd/m2), frames %u\n", smoothed, PqToNits(smoothed),
                PqToNits(quantized), words[2]);
    CHECK(words[2] == 30 && std::abs(smoothed - quantized) < 0.002);
    ReleaseCom(staging); ReleaseCom(reference_staging); ReleaseCom(heap); ReleaseCom(result); ReleaseCom(expected); ReleaseCom(output); ReleaseCom(source);
    ReleaseCom(compare); ReleaseCom(compare_root); color.Release();
    ReleaseCom(gpu.list); ReleaseCom(gpu.allocator); ReleaseCom(gpu.fence); ReleaseCom(gpu.queue); ReleaseCom(gpu.device);
    std::printf("Tone mapping: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
