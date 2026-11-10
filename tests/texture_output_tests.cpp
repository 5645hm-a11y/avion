// Texture output (API 13, 14; docs/sdk.md, "Texture output: API 13", "API 14"), through the C99 SDK as a host renderer uses it: a
// Direct3D 11 or Direct3D 12 device of its own (a game engine's, a compositor's) on the engine's adapter opens the three shared
// textures and the two shared fences, and per frame acquires the newest picture, waits for the engine's fence on its own GPU
// timeline (no CPU wait), copies from the texture, signals its release fence and releases the texture.
//   media_texture_output_tests <fixture.mp4> [--d3d12] [--format rgba8|pq10|scrgb16] [--hdr] [--host-mode]
// Windowed engine (the default), on playback_chapters.mp4 (three flat scenes: dark, bright, mid, cuts at 3 s and 6 s):
// 1. The config: MEDIA_OUTPUT_TEXTURE refused with a parent window, a size outside 16..8192, an unknown flag or format, a format
//    without texture output; the calls refused before open (MEDIA_ERROR_STATE).
// 2. Playing into 320 x 180 textures: three textures of that size and the format asked for, on the adapter the LUID names, the
//    colour space and SDR white of the format; frames with rising sequences and fence values; the mean luma of the picture's
//    centre in the host's copy (decoded from PQ or scRGB back to display-referred values for the HDR formats) has each scene's
//    level; the side bars are black (the picture is fitted, 1:1, into 16:9).
// 3. media_engine_resize_output to 256 x 144 while playing: a new generation, new handles of the new size, frames continuing.
// 4. 0 allocations in the process over a second of the host's per-frame calls (the engine's threads included).
// --d3d12: the host is a Direct3D 12 device (ID3D12Device::OpenSharedHandle for the textures and both fences, its own direct
//    queue waiting for the engine's fence and signalling its release fence) instead of Direct3D 11.
// --hdr: an HDR10 fixture (PQ, BT.2020, up to ~7,000 cd/m2): in pq10 or scrgb16 the host reads light above SDR white (more than
//    1,000 cd/m2 at the brightest), which an 8-bit tone-mapped texture can never carry.
// --host-mode (API 14): MEDIA_PLAYBACK_HOST with MEDIA_OUTPUT_TEXTURE: the host steps the engine on its own thread (and pumps its
//    audio); the engine decodes on the GPU (media_query_video_decoder says MEDIA_DECODER_HARDWARE) and draws into the textures;
//    on_present says MEDIA_PICTURE_TEXTURE. Chapters: the levels of the three scenes through two seeks ("go to 0:04", "go to
//    0:07"); the process's CPU time while playing; 0 allocations over a second of steps and host calls.
// Prints the frame count and the levels. Exit 0 when every check passed.
#include "media/c_api.h"
#include "allocation_probe.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)

template <class T> void Release(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }

double Seconds() {
    LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}
std::int64_t Now100ns() { return static_cast<std::int64_t>(Seconds() * 1e7); }
double ProcessCpuSeconds() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    const auto add = [](FILETIME t) { return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e7; };
    return add(kernel) + add(user);
}

// ---- The pixels the host reads, back to display-referred 8-bit values (the test's eyes) ----
struct Mode { bool d3d12 = false, hdr = false, host = false; std::uint32_t format = MEDIA_OUTPUT_FORMAT_RGBA8; };
std::uint32_t Dxgi(std::uint32_t format) { return format == MEDIA_OUTPUT_FORMAT_PQ10 ? 24u : format == MEDIA_OUTPUT_FORMAT_SCRGB16 ? 10u : 28u; }
unsigned PixelBytes(std::uint32_t format) { return format == MEDIA_OUTPUT_FORMAT_SCRGB16 ? 8u : 4u; }
double PqToNits(double e) {
    const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const double p = std::pow((std::max)(e, 0.0), 1.0 / m2);
    return 10000.0 * std::pow((std::max)((p - c1) / (c2 - c3 * p), 0.0), 1.0 / m1);
}
double Half(std::uint16_t h) {
    const int exponent = (h >> 10) & 31, mantissa = h & 1023; const double sign = (h & 0x8000) ? -1.0 : 1.0;
    if (exponent == 0) return sign * std::ldexp(mantissa, -24);
    if (exponent == 31) return sign * 65504.0;
    return sign * std::ldexp(1024 + mantissa, exponent - 25);
}
// A pixel's light, cd/m2, linear BT.709 (the HDR formats), or its 8-bit R G B (rgba8): `rgb` gets display-referred 0..255 values
// (an HDR format's light divided by the SDR white, 203 cd/m2, then the BT.1886 2.4 gamma), `peak` its brightest channel's light.
void Pixel(std::uint32_t format, const unsigned char* p, double rgb[3], double& peak) {
    double light[3];
    if (format == MEDIA_OUTPUT_FORMAT_RGBA8) {
        for (int c = 0; c < 3; ++c) rgb[c] = p[c];
        peak = 0; return;
    }
    if (format == MEDIA_OUTPUT_FORMAT_PQ10) {
        std::uint32_t v; std::memcpy(&v, p, 4);
        const double n[3] = {PqToNits((v & 1023) / 1023.0), PqToNits(((v >> 10) & 1023) / 1023.0), PqToNits(((v >> 20) & 1023) / 1023.0)};
        // BT.2020 -> BT.709 (ITU-R BT.2087), linear light.
        light[0] = 1.660491 * n[0] - 0.587641 * n[1] - 0.072850 * n[2];
        light[1] = -0.124550 * n[0] + 1.132900 * n[1] - 0.008349 * n[2];
        light[2] = -0.018151 * n[0] - 0.100579 * n[1] + 1.118730 * n[2];
    } else {
        std::uint16_t h[4]; std::memcpy(h, p, 8);
        for (int c = 0; c < 3; ++c) light[c] = Half(h[c]) * 80.0; // scRGB: 1.0 = 80 cd/m2.
    }
    peak = (std::max)(light[0], (std::max)(light[1], light[2]));
    for (int c = 0; c < 3; ++c) rgb[c] = 255.0 * std::pow((std::min)((std::max)(light[c] / 203.0, 0.0), 1.0), 1.0 / 2.4);
}
// The test's own CPU time decoding pixels (its eyes: a real host samples the texture on its GPU), kept apart from the engine's.
double g_analysis = 0;
// The mean BT.601 luma of the 32 x 32 centre, the brightest channel at the left edge (the bars), the brightest light anywhere.
void Measure(std::uint32_t format, const unsigned char* pixels, unsigned pitch, unsigned width, unsigned height, unsigned& centre, unsigned& edge, double& peak) {
    const double began = Seconds();
    const unsigned bytes = PixelBytes(format);
    double sum = 0, rgb[3], light = 0; peak = 0;
    for (unsigned y = height / 2 - 16; y != height / 2 + 16; ++y)
        for (unsigned x = width / 2 - 16; x != width / 2 + 16; ++x) {
            Pixel(format, pixels + static_cast<size_t>(y) * pitch + x * bytes, rgb, light);
            sum += 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
        }
    centre = static_cast<unsigned>(sum / 1024 + 0.5);
    Pixel(format, pixels + static_cast<size_t>(height / 2) * pitch + bytes, rgb, light);
    edge = static_cast<unsigned>((std::max)(rgb[0], (std::max)(rgb[1], rgb[2])) + 0.5);
    for (unsigned y = 0; y < height; y += 2)
        for (unsigned x = 0; x < width; x += 2) { Pixel(format, pixels + static_cast<size_t>(y) * pitch + x * bytes, rgb, light); peak = (std::max)(peak, light); }
    g_analysis += Seconds() - began;
}

// ---- The host's renderers ----
struct HostRenderer {
    unsigned width = 0, height = 0, generation = 0;
    std::uint32_t format = 0;
    std::uint64_t host_value = 0;
    virtual ~HostRenderer() = default;
    virtual bool Create(const media_output_textures_t& d) = 0;
    virtual bool Open(const media_output_textures_t& d) = 0;
    // One frame: wait (on the GPU) for the engine's drawing, copy, signal the host fence, hand the texture back; then measure the
    // copy (the test's CPU wait for its own copy, not the host's).
    virtual bool Frame(media_engine_t* e, const media_output_frame_t& f, unsigned& centre, unsigned& edge, double& peak) = 0;
};
bool Adapter(const media_output_textures_t& d, IDXGIAdapter1** adapter) {
    IDXGIFactory4* factory = nullptr;
    LUID luid{}; luid.LowPart = d.adapter_luid_low; luid.HighPart = static_cast<LONG>(d.adapter_luid_high);
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter));
    Release(factory);
    return SUCCEEDED(hr);
}

// Direct3D 11 (API 13's host): OpenSharedResource1, OpenSharedFence, ID3D11DeviceContext4::Wait / Signal, a staging copy.
struct Host11 final : HostRenderer {
    ID3D11Device5* device = nullptr;
    ID3D11DeviceContext4* context = nullptr;
    ID3D11Texture2D* textures[3]{};
    ID3D11Fence* fence = nullptr;      // The engine's: a frame's fence_value is signalled once its picture is drawn.
    ID3D11Fence* host_fence = nullptr; // The host's: signalled once its reads of a texture are queued before them.
    ID3D11Texture2D* staging = nullptr;
    bool Create(const media_output_textures_t& d) override {
        IDXGIAdapter1* adapter = nullptr;
        ID3D11Device* base = nullptr; ID3D11DeviceContext* immediate = nullptr;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = Adapter(d, &adapter) ? S_OK : E_FAIL;
        if (SUCCEEDED(hr)) hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &base, nullptr, &immediate);
        if (SUCCEEDED(hr)) hr = base->QueryInterface(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) hr = immediate->QueryInterface(IID_PPV_ARGS(&context));
        Release(immediate); Release(base); Release(adapter);
        if (FAILED(hr)) { std::printf("host device (D3D11): 0x%08lx\n", static_cast<unsigned long>(hr)); return false; }
        return Open(d);
    }
    bool Open(const media_output_textures_t& d) override {
        for (auto*& t : textures) Release(t);
        Release(fence); Release(host_fence); Release(staging);
        HRESULT hr = S_OK;
        int stage = 0;
        for (unsigned i = 0; i != 3 && SUCCEEDED(hr); ++i) { hr = device->OpenSharedResource1(static_cast<HANDLE>(d.textures[i]), IID_PPV_ARGS(&textures[i])); stage = 1 + static_cast<int>(i); }
        if (SUCCEEDED(hr)) { hr = device->OpenSharedFence(static_cast<HANDLE>(d.fence), IID_PPV_ARGS(&fence)); stage = 4; }
        if (SUCCEEDED(hr)) { hr = device->OpenSharedFence(static_cast<HANDLE>(d.host_fence), IID_PPV_ARGS(&host_fence)); stage = 5; }
        if (FAILED(hr)) std::printf("host open (D3D11): stage %d\n", stage);
        D3D11_TEXTURE2D_DESC s{}; if (SUCCEEDED(hr)) textures[0]->GetDesc(&s);
        CHECK(SUCCEEDED(hr) && s.Width == d.width && s.Height == d.height && s.Format == static_cast<DXGI_FORMAT>(d.format));
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
        if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&s, nullptr, &staging);
        // The host fence carries on from its last value (the engine waits for values the host signals, in order).
        width = d.width; height = d.height; generation = d.generation; format = d.format;
        if (FAILED(hr)) std::printf("host open (D3D11): 0x%08lx\n", static_cast<unsigned long>(hr));
        return SUCCEEDED(hr);
    }
    bool Frame(media_engine_t* e, const media_output_frame_t& f, unsigned& centre, unsigned& edge, double& peak) override {
        if (FAILED(context->Wait(fence, f.fence_value))) return false;
        context->CopyResource(staging, textures[f.index]);
        ++host_value;
        if (FAILED(context->Signal(host_fence, host_value))) return false;
        CHECK(media_engine_release_output_frame(e, f.index, host_value) == MEDIA_OK);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return false;
        const std::uint32_t media_format = format == 24 ? MEDIA_OUTPUT_FORMAT_PQ10 : format == 10 ? MEDIA_OUTPUT_FORMAT_SCRGB16 : MEDIA_OUTPUT_FORMAT_RGBA8;
        Measure(media_format, static_cast<const unsigned char*>(m.pData), m.RowPitch, width, height, centre, edge, peak);
        context->Unmap(staging, 0);
        return true;
    }
    ~Host11() override {
        for (auto*& t : textures) Release(t);
        Release(fence); Release(host_fence); Release(staging); Release(context); Release(device);
    }
};

// Direct3D 12 (API 14's verification): ID3D12Device::OpenSharedHandle for the three textures and both fences, its own direct queue
// waiting on the GPU for the engine's fence value, copying into a readback buffer and signalling the host fence.
struct Host12 final : HostRenderer {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* textures[3]{};
    ID3D12Fence* fence = nullptr;      // The engine's frames fence.
    ID3D12Fence* host_fence = nullptr; // The host's release fence.
    ID3D12Fence* own = nullptr;        // The test's: when its copy is readable on the CPU.
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    HANDLE event = nullptr;
    std::uint64_t own_value = 0;
    bool Create(const media_output_textures_t& d) override {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT hr = Adapter(d, &adapter) ? S_OK : E_FAIL;
        if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        Release(adapter);
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(hr) || !event) { std::printf("host device (D3D12): 0x%08lx\n", static_cast<unsigned long>(hr)); return false; }
        return Open(d);
    }
    bool Open(const media_output_textures_t& d) override {
        for (auto*& t : textures) Release(t);
        Release(fence); Release(host_fence); Release(readback);
        HRESULT hr = S_OK;
        for (unsigned i = 0; i != 3 && SUCCEEDED(hr); ++i) hr = device->OpenSharedHandle(static_cast<HANDLE>(d.textures[i]), IID_PPV_ARGS(&textures[i]));
        if (SUCCEEDED(hr)) hr = device->OpenSharedHandle(static_cast<HANDLE>(d.fence), IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) hr = device->OpenSharedHandle(static_cast<HANDLE>(d.host_fence), IID_PPV_ARGS(&host_fence));
        D3D12_RESOURCE_DESC s{}; if (SUCCEEDED(hr)) s = textures[0]->GetDesc();
        CHECK(SUCCEEDED(hr) && s.Width == d.width && s.Height == d.height && s.Format == static_cast<DXGI_FORMAT>(d.format));
        UINT64 bytes = 0;
        if (SUCCEEDED(hr)) device->GetCopyableFootprints(&s, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes; buffer.Height = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        width = d.width; height = d.height; generation = d.generation; format = d.format;
        if (FAILED(hr)) std::printf("host open (D3D12): 0x%08lx\n", static_cast<unsigned long>(hr));
        return SUCCEEDED(hr);
    }
    bool Frame(media_engine_t* e, const media_output_frame_t& f, unsigned& centre, unsigned& edge, double& peak) override {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) return false;
        auto* texture = textures[f.index];
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION to{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = layout;
        D3D12_TEXTURE_COPY_LOCATION from{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter); list->ResourceBarrier(1, &b);
        if (FAILED(list->Close())) return false;
        // The engine's drawing first, on this queue's timeline; then the copy; then the release value (nothing waits on a CPU).
        if (FAILED(queue->Wait(fence, f.fence_value))) return false;
        ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
        ++host_value;
        if (FAILED(queue->Signal(host_fence, host_value))) return false;
        CHECK(media_engine_release_output_frame(e, f.index, host_value) == MEDIA_OK);
        ++own_value;
        if (FAILED(queue->Signal(own, own_value))) return false;
        if (own->GetCompletedValue() < own_value) {
            if (FAILED(own->SetEventOnCompletion(own_value, event)) || WaitForSingleObject(event, 2000) != WAIT_OBJECT_0) return false;
        }
        void* mapped = nullptr; const D3D12_RANGE all{0, static_cast<SIZE_T>(layout.Footprint.RowPitch) * height};
        if (FAILED(readback->Map(0, &all, &mapped))) return false;
        const std::uint32_t media_format = format == 24 ? MEDIA_OUTPUT_FORMAT_PQ10 : format == 10 ? MEDIA_OUTPUT_FORMAT_SCRGB16 : MEDIA_OUTPUT_FORMAT_RGBA8;
        Measure(media_format, static_cast<const unsigned char*>(mapped) + layout.Offset, layout.Footprint.RowPitch, width, height, centre, edge, peak);
        const D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
        return true;
    }
    ~Host12() override {
        if (queue && own) { // Idle before release.
            ++own_value;
            if (SUCCEEDED(queue->Signal(own, own_value)) && own->GetCompletedValue() < own_value && SUCCEEDED(own->SetEventOnCompletion(own_value, event)))
                (void)WaitForSingleObject(event, 2000);
        }
        for (auto*& t : textures) Release(t);
        Release(fence); Release(host_fence); Release(own); Release(readback); Release(list); Release(allocator); Release(queue); Release(device);
        if (event) CloseHandle(event);
    }
};
HostRenderer* MakeHost(const Mode& m) { return m.d3d12 ? static_cast<HostRenderer*>(new Host12) : static_cast<HostRenderer*>(new Host11); }

void TextureConfig(media_engine_config_t& c, unsigned width, unsigned height, std::uint32_t format) {
    media_engine_config_init(&c);
    c.modules = MEDIA_MODULE_ALL & ~static_cast<uint32_t>(MEDIA_MODULE_VOICE | MEDIA_MODULE_HUD); // The host draws its own controls.
    c.visible = 0; c.output_flags = MEDIA_OUTPUT_TEXTURE; c.output_width = width; c.output_height = height; c.output_format = format;
}

// The engine's thread (the windowed engine): create, open, run, close; destroyed once the host has finished with it.
struct EngineRun {
    const char* path = nullptr;
    unsigned char* storage = nullptr;
    std::uint32_t format = 0;
    media_engine_t* volatile engine = nullptr;
    volatile LONG opened = 0, done = 0, release = 0;
    media_status create_status = MEDIA_OK, open_status = MEDIA_OK, run_status = MEDIA_OK;
};
DWORD WINAPI EngineThread(void* p) {
    auto* r = static_cast<EngineRun*>(p);
    media_engine_config_t config; TextureConfig(config, 320, 180, r->format);
    media_engine_t* e = nullptr;
    r->create_status = media_engine_create(&config, r->storage, media_engine_storage_bytes(), &e);
    if (r->create_status != MEDIA_OK) { InterlockedExchange(&r->opened, 1); InterlockedExchange(&r->done, 1); return 0; }
    r->open_status = media_engine_open(e, r->path);
    r->engine = e; InterlockedExchange(&r->opened, 1);
    if (r->open_status == MEDIA_OK) { r->run_status = media_engine_run(e); (void)media_engine_close(e); }
    InterlockedExchange(&r->done, 1);
    while (!InterlockedCompareExchange(&r->release, 0, 0)) Sleep(1);
    media_engine_destroy(e);
    return 0;
}

void ConfigChecks(unsigned char* storage) {
    media_engine_config_t c; media_engine_t* e = nullptr;
    HWND desktop = GetDesktopWindow();
    TextureConfig(c, 320, 180, 0); c.parent_window = desktop;
    CHECK(media_engine_create(&c, storage, media_engine_storage_bytes(), &e) == MEDIA_ERROR_INVALID_ARGUMENT);
    TextureConfig(c, 8, 180, 0);
    CHECK(media_engine_create(&c, storage, media_engine_storage_bytes(), &e) == MEDIA_ERROR_INVALID_ARGUMENT);
    TextureConfig(c, 320, 8193, 0);
    CHECK(media_engine_create(&c, storage, media_engine_storage_bytes(), &e) == MEDIA_ERROR_INVALID_ARGUMENT);
    TextureConfig(c, 320, 180, 0); c.output_flags = 2u;
    CHECK(media_engine_create(&c, storage, media_engine_storage_bytes(), &e) == MEDIA_ERROR_INVALID_ARGUMENT);
    TextureConfig(c, 320, 180, 3u); // API 14: an unknown format.
    CHECK(media_engine_create(&c, storage, media_engine_storage_bytes(), &e) == MEDIA_ERROR_INVALID_ARGUMENT);
    TextureConfig(c, 320, 180, MEDIA_OUTPUT_FORMAT_PQ10); c.output_flags = 0; // A format without texture output.
    CHECK(media_engine_create(&c, storage, media_engine_storage_bytes(), &e) == MEDIA_ERROR_INVALID_ARGUMENT);
    // API 14: host mode with texture output is its GPU decoding (refused in API 13).
    TextureConfig(c, 320, 180, 0); c.playback = MEDIA_PLAYBACK_HOST;
    CHECK(media_engine_create(&c, storage, media_engine_storage_bytes(), &e) == MEDIA_OK && e);
    if (e) {
        media_output_textures_t t{}; t.struct_size = sizeof t;
        CHECK(media_engine_query_output_textures(e, &t) == MEDIA_ERROR_STATE && media_query_video_decoder(e) == MEDIA_DECODER_NONE);
        media_engine_destroy(e); e = nullptr;
    }
    // Created, not opened: every call refused with MEDIA_ERROR_STATE; bad arguments first.
    TextureConfig(c, 320, 180, 0);
    CHECK(media_engine_create(&c, storage, media_engine_storage_bytes(), &e) == MEDIA_OK && e);
    if (!e) return;
    media_output_textures_t t{}; t.struct_size = sizeof t;
    media_output_frame_t f{}; f.struct_size = sizeof f;
    CHECK(media_engine_query_output_textures(e, &t) == MEDIA_ERROR_STATE);
    CHECK(media_engine_acquire_output_frame(e, &f) == MEDIA_ERROR_STATE);
    CHECK(media_engine_release_output_frame(e, 0, 0) == MEDIA_ERROR_STATE);
    CHECK(media_engine_resize_output(e, 256, 144) == MEDIA_ERROR_STATE);
    CHECK(media_engine_release_output_frame(e, 3, 0) == MEDIA_ERROR_INVALID_ARGUMENT);
    CHECK(media_engine_resize_output(e, 15, 144) == MEDIA_ERROR_INVALID_ARGUMENT);
    t.struct_size = 8; CHECK(media_engine_query_output_textures(e, &t) == MEDIA_ERROR_INVALID_ARGUMENT);
    f.struct_size = 8; CHECK(media_engine_acquire_output_frame(e, &f) == MEDIA_ERROR_INVALID_ARGUMENT);
    t.struct_size = 72; CHECK(media_engine_query_output_textures(e, &t) == MEDIA_ERROR_STATE); // API 13's struct: accepted.
    f.struct_size = 32; CHECK(media_engine_acquire_output_frame(e, &f) == MEDIA_ERROR_STATE);
    media_engine_destroy(e);
}

// What a run saw: the chapters' three scenes (by the frame's pts) and the brightest light of an HDR fixture.
struct Seen {
    unsigned frames = 0, after_resize = 0, out_of_order = 0, wrong[3]{}, seen[3]{}, bars_lit = 0, resized_generation = 0, opened_generation = 0;
    unsigned level_min[3] = {255, 255, 255}, level_max[3]{};
    std::uint32_t last_sequence = 0; std::uint64_t last_value = 0;
    double peak = 0;
    unsigned hdr_frames = 0, wrong_transfer = 0, unknown_transfer = 0;
};
const char* FormatName(std::uint32_t f) { return f == MEDIA_OUTPUT_FORMAT_PQ10 ? "pq10" : f == MEDIA_OUTPUT_FORMAT_SCRGB16 ? "scrgb16" : "rgba8"; }
// One acquired frame through the host (reopening the textures of a new generation first). False: nothing taken.
bool TakeFrame(media_engine_t* e, HostRenderer& host, const Mode& m, Seen& s, unsigned resized_width) {
    media_output_frame_t f{}; f.struct_size = sizeof f;
    const media_status status = media_engine_acquire_output_frame(e, &f);
    if (status != MEDIA_OK) { CHECK(status == MEDIA_ERROR_EMPTY || status == MEDIA_ERROR_STATE); return false; }
    if (f.generation != host.generation) {
        // A resize: the textures of the new generation (a frame of the old one needs no release: its textures are gone).
        media_output_textures_t n{}; n.struct_size = sizeof n;
        if (media_engine_query_output_textures(e, &n) != MEDIA_OK || n.generation != f.generation) { (void)media_engine_release_output_frame(e, f.index, host.host_value); return false; }
        CHECK(host.Open(n)); s.resized_generation = n.generation;
        CHECK(n.width == resized_width && n.height == resized_width * 9 / 16 && n.textures[0] && n.fence); // (A handle's value may be reused.)
    }
    unsigned centre = 0, edge = 0; double peak = 0;
    CHECK(host.Frame(e, f, centre, edge, peak));
    ++s.frames; if (s.resized_generation) ++s.after_resize;
    if (f.sequence <= s.last_sequence || f.fence_value <= s.last_value) ++s.out_of_order;
    s.last_sequence = f.sequence; s.last_value = f.fence_value;
    if (edge > 12) ++s.bars_lit;
    if (m.hdr) {
        s.peak = (std::max)(s.peak, peak); ++s.hdr_frames;
        if (!f.transfer) ++s.unknown_transfer; // The windowed engine's first composite, before any picture: 0, unknown.
        else if (f.transfer != 16) ++s.wrong_transfer; // The PQ fixture's frames say PQ (API 14's frame struct).
        return true;
    }
    if (!f.transfer) ++s.unknown_transfer;
    else if (f.transfer == 16 || f.transfer == 18) ++s.wrong_transfer; // The chapters fixture is SDR (its resolved transfer).
    const double t = static_cast<double>(f.pts_100ns) / 1e7;
    const int scene = t < 2.8 && t > 0.2 ? 0 : t > 3.2 && t < 5.8 ? 1 : t > 6.2 && t < 8.8 ? 2 : -1;
    if (scene >= 0) {
        // Each scene's mean level at the centre: its luma plus the fixture's 0..23 pattern, from video range to full range
        // (24..47 -> 23, 196..219 -> 223, 110..133 -> 123 on average).
        const unsigned expected[3] = {23, 223, 123};
        ++s.seen[scene];
        if (centre + 20 < expected[scene] || centre > expected[scene] + 20) ++s.wrong[scene];
        if (centre < s.level_min[scene]) s.level_min[scene] = centre;
        if (centre > s.level_max[scene]) s.level_max[scene] = centre;
    }
    return true;
}
void PrintSeen(const Seen& s, const Mode& m, double played) {
    std::printf("played %.1f s: %u frames to the host (%u after the resize, generation %u), out of order %u, lit bars %u, transfer wrong %u, unknown %u\n",
                played, s.frames, s.after_resize, s.resized_generation, s.out_of_order, s.bars_lit, s.wrong_transfer, s.unknown_transfer);
    if (m.hdr) std::printf("HDR: brightest light the host read %.0f cd/m2 over %u frames (SDR white 203)\n", s.peak, s.hdr_frames);
    else std::printf("levels at the centre: dark %u..%u (%u frames), bright %u..%u (%u), mid %u..%u (%u); wrong %u/%u/%u\n",
                     s.level_min[0], s.level_max[0], s.seen[0], s.level_min[1], s.level_max[1], s.seen[1], s.level_min[2], s.level_max[2], s.seen[2],
                     s.wrong[0], s.wrong[1], s.wrong[2]);
}
bool CheckTextures(const media_output_textures_t& d, const Mode& m) {
    const std::uint32_t space = m.format == MEDIA_OUTPUT_FORMAT_PQ10 ? 12u : m.format == MEDIA_OUTPUT_FORMAT_SCRGB16 ? 1u : 0u;
    CHECK(d.count == 3 && d.width == 320 && d.height == 180 && d.format == Dxgi(m.format) && d.generation != 0 && d.color_space == space);
    CHECK(m.format == MEDIA_OUTPUT_FORMAT_RGBA8 ? d.sdr_white_nits == 0.0f : d.sdr_white_nits == 203.0f);
    CHECK(d.textures[0] && d.textures[1] && d.textures[2] && d.fence && d.host_fence);
    std::printf("textures (%s host, %s): %ux%u DXGI format %u, colour space %u, generation %u, adapter %08x:%08x\n", m.d3d12 ? "D3D12" : "D3D11",
                FormatName(m.format), d.width, d.height, d.format, d.color_space, d.generation, d.adapter_luid_high, d.adapter_luid_low);
    return d.textures[0] != nullptr;
}

void Windowed(const char* path, unsigned char* storage, const Mode& m) {
    EngineRun r; r.path = path; r.storage = storage; r.format = m.format;
    HANDLE thread = CreateThread(nullptr, 0, EngineThread, &r, 0, nullptr);
    while (!InterlockedCompareExchange(&r.opened, 0, 0)) Sleep(1);
    media_engine_t* e = r.engine;
    CHECK(r.create_status == MEDIA_OK && r.open_status == MEDIA_OK && e);
    if (!e || r.open_status != MEDIA_OK) { InterlockedExchange(&r.release, 1); WaitForSingleObject(thread, 10000); CloseHandle(thread); return; }
    CHECK(media_engine_post_command(e, "mute", 1) == MEDIA_OK);
    CHECK(media_query_video_decoder(e) == MEDIA_DECODER_HARDWARE);
    media_output_textures_t d{}; d.struct_size = sizeof d;
    media_status queried = MEDIA_ERROR_EMPTY;
    for (const double until = Seconds() + 5; queried == MEDIA_ERROR_EMPTY && Seconds() < until; Sleep(1)) queried = media_engine_query_output_textures(e, &d);
    CHECK(queried == MEDIA_OK);
    HostRenderer* host = MakeHost(m);
    const bool ready = queried == MEDIA_OK && CheckTextures(d, m) && host->Create(d);
    CHECK(ready);
    Seen s; bool asked_resize = false, probed = false; std::uint64_t allocations = 0; unsigned probed_frames = 0;
    double probe_end = 0;
    const double began = Seconds();
    while (ready && !InterlockedCompareExchange(&r.done, 0, 0) && Seconds() - began < 20) {
        if (!TakeFrame(e, *host, m, s, 256)) { Sleep(1); continue; }
        const double t = Seconds() - began;
        // A second of the host's calls with the allocation probe on (from 1.5 s), then the resize (from 4.5 s; not for the short HDR fixture).
        if (!probed && t > 1.5) { probed = true; probe_end = Seconds() + 1; probed_frames = s.frames; allocation_probe::Begin(); }
        if (probe_end && Seconds() >= probe_end) { allocations = allocation_probe::End(); probe_end = 0; probed_frames = s.frames - probed_frames; }
        if (!asked_resize && !m.hdr && t > 4.5) { asked_resize = true; CHECK(media_engine_resize_output(e, 256, 144) == MEDIA_OK); }
    }
    if (probe_end) { allocations = allocation_probe::End(); probed_frames = s.frames - probed_frames; }
    const double played = Seconds() - began;
    CHECK(InterlockedCompareExchange(&r.done, 0, 0) && r.run_status == MEDIA_OK);
    CHECK(s.out_of_order == 0 && (m.hdr || s.bars_lit == 0) && s.wrong_transfer == 0 && s.unknown_transfer <= 2); // (The 4K HDR fixture is 16:9: no bars.)
    if (m.hdr) {
        CHECK(s.hdr_frames >= 40 && s.peak > 1000.0); // HDR kept: light far above SDR white reaches the host.
    } else {
        CHECK(s.frames >= 150);
        CHECK(s.seen[0] >= 30 && s.seen[1] >= 30 && s.seen[2] >= 30 && s.wrong[0] == 0 && s.wrong[1] == 0 && s.wrong[2] == 0);
        CHECK(s.resized_generation != 0 && s.resized_generation != d.generation && s.after_resize >= 40);
    }
    CHECK(probed && allocations == 0 && probed_frames >= 15);
    PrintSeen(s, m, played);
    std::printf("allocations over %u frames of host calls: %llu\n", probed_frames, static_cast<unsigned long long>(allocations));
    delete host;
    InterlockedExchange(&r.release, 1);
    WaitForSingleObject(thread, 10000); CloseHandle(thread);
}

// ---- Host mode (API 14): the host steps the engine; the engine decodes on the GPU into the textures ----
struct Presented { unsigned texture = 0, planes = 0, other = 0; };
void OnPresent(void* context, const media_picture_t* p) {
    auto* c = static_cast<Presented*>(context);
    if (p->flags & MEDIA_PICTURE_TEXTURE) ++c->texture; else if (p->luma) ++c->planes; else ++c->other;
}
void HostMode(const char* path, unsigned char* storage, const Mode& m) {
    media_engine_config_t c; TextureConfig(c, 320, 180, m.format);
    c.playback = MEDIA_PLAYBACK_HOST; c.audio_rate = 48000; c.audio_channels = 2;
    Presented presented;
    c.on_present = &OnPresent; c.on_present_context = &presented;
    media_engine_t* e = nullptr;
    CHECK(media_engine_create(&c, storage, media_engine_storage_bytes(), &e) == MEDIA_OK && e);
    if (!e) return;
    const media_status opened = media_engine_open(e, path);
    CHECK(opened == MEDIA_OK);
    if (opened != MEDIA_OK) { std::printf("host open: stage %u\n", media_engine_last_error(e, nullptr)); media_engine_destroy(e); return; }
    const std::uint32_t decoder = media_query_video_decoder(e);
    std::printf("host mode: video decoder %u (3: the GPU)\n", decoder);
    CHECK(decoder == MEDIA_DECODER_HARDWARE);
    CHECK(media_engine_command(e, "mute") == MEDIA_OK);
    static float pump[2 * 480];
    HostRenderer* host = MakeHost(m);
    bool ready = false;
    Seen s; std::uint64_t allocations = 0; unsigned probed_frames = 0, steps = 0; bool probed = false, sought[2]{}, resized = false;
    double probe_end = 0, cpu_began = 0, cpu_played = 0;
    const double began = Seconds(); double audio_clock = began;
    media_status status = MEDIA_OK;
    while (Seconds() - began < 30) {
        const std::int64_t now = Now100ns();
        status = media_engine_step(e, now); ++steps;
        if (status != MEDIA_OK) break;
        // 10 ms of audio for every 10 ms gone (the host's audio callback, here on the same thread).
        while (Seconds() - audio_clock >= 0.01) { (void)media_engine_pump_audio(e, pump, 480, now); audio_clock += 0.01; }
        if (!ready) {
            media_output_textures_t d{}; d.struct_size = sizeof d;
            if (media_engine_query_output_textures(e, &d) == MEDIA_OK) {
                ready = CheckTextures(d, m) && host->Create(d); CHECK(ready); s.opened_generation = d.generation; cpu_began = ProcessCpuSeconds(); g_analysis = 0;
            }
        }
        if (ready) (void)TakeFrame(e, *host, m, s, 256);
        const double t = Seconds() - began;
        if (ready && !probed && t > 1.0) { probed = true; probe_end = Seconds() + 1; probed_frames = s.frames; allocation_probe::Begin(); }
        if (probe_end && Seconds() >= probe_end) { allocations = allocation_probe::End(); probe_end = 0; probed_frames = s.frames - probed_frames; }
        if (!m.hdr && !sought[0] && t > 2.4) { sought[0] = true; CHECK(media_engine_command(e, "go to 0:04") == MEDIA_OK); }
        if (!m.hdr && !sought[1] && t > 4.6) { sought[1] = true; CHECK(media_engine_command(e, "go to 0:07") == MEDIA_OK); }
        if (!m.hdr && !resized && t > 5.4) { resized = true; CHECK(media_engine_resize_output(e, 256, 144) == MEDIA_OK); } // A new generation, while playing.
        Sleep(1);
    }
    if (probe_end) { allocations = allocation_probe::End(); probed_frames = s.frames - probed_frames; }
    const double played = Seconds() - began;
    cpu_played = ProcessCpuSeconds() - cpu_began;
    CHECK(status == MEDIA_ERROR_EMPTY); // The end of the media.
    CHECK(s.out_of_order == 0 && (m.hdr || s.bars_lit == 0) && s.wrong_transfer == 0 && s.unknown_transfer == 0); // Host mode: every frame is a picture.
    CHECK(presented.texture >= 40 && presented.planes == 0);
    media_engine_stats_t stats{};
    CHECK(media_engine_stats(e, &stats) == MEDIA_OK && stats.arena_capacity_bytes == media_query_arena_budget(&c) && stats.threads == 0);
    if (m.hdr) CHECK(s.hdr_frames >= 40 && s.peak > 1000.0);
    else {
        // Dark from the start to the first seek, bright from 0:04 to the second, mid from 0:07 to the end (three cuts by seeking).
        CHECK(s.seen[0] >= 30 && s.seen[1] >= 30 && s.seen[2] >= 30 && s.wrong[0] == 0 && s.wrong[1] == 0 && s.wrong[2] == 0);
        CHECK(s.resized_generation != 0 && s.resized_generation != s.opened_generation && s.after_resize >= 20);
    }
    CHECK(probed && allocations == 0 && probed_frames >= 15);
    PrintSeen(s, m, played);
    const double engine_cpu = cpu_played - g_analysis; // Less the test's pixel decoding (single-threaded and CPU-bound: its wall time).
    std::printf("host mode: %u steps, %u pictures presented into textures (%u with planes); process CPU %.2f s over %.2f s of playing, the test's "
                "pixel analysis %.2f s of it: the engine and the host's GPU calls %.2f s (%.1f %% of one core)\n",
                steps, presented.texture, presented.planes, cpu_played, played, g_analysis, engine_cpu, 100.0 * engine_cpu / (played > 0 ? played : 1));
    std::printf("allocations over %u frames of steps and host calls: %llu\n", probed_frames, static_cast<unsigned long long>(allocations));
    delete host;
    (void)media_engine_close(e);
    media_engine_destroy(e);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: media_texture_output_tests fixture.mp4 [--d3d12] [--format rgba8|pq10|scrgb16] [--hdr] [--host-mode]\n"); return 2; }
    Mode m;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--d3d12")) m.d3d12 = true;
        else if (!std::strcmp(argv[i], "--hdr")) m.hdr = true;
        else if (!std::strcmp(argv[i], "--host-mode")) m.host = true;
        else if (!std::strcmp(argv[i], "--format") && i + 1 < argc) {
            ++i; m.format = !std::strcmp(argv[i], "pq10") ? MEDIA_OUTPUT_FORMAT_PQ10 : !std::strcmp(argv[i], "scrgb16") ? MEDIA_OUTPUT_FORMAT_SCRGB16 : MEDIA_OUTPUT_FORMAT_RGBA8;
        } else { std::printf("unknown argument %s\n", argv[i]); return 2; }
    }
    CHECK(media_api_version() == 17u);
    const size_t bytes = media_engine_storage_bytes(), alignment = media_engine_storage_alignment();
    auto* raw = static_cast<unsigned char*>(VirtualAlloc(nullptr, bytes + alignment, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!raw) return 1;
    auto* storage = reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(raw) + alignment - 1) / alignment * alignment);
    std::printf("mode: %s, %s host, format %s%s\n", m.host ? "host mode (GPU decoding)" : "windowed engine", m.d3d12 ? "Direct3D 12" : "Direct3D 11", FormatName(m.format),
                m.hdr ? ", HDR fixture" : "");
    ConfigChecks(storage);
    if (m.host) HostMode(argv[1], storage, m); else Windowed(argv[1], storage, m);
    VirtualFree(raw, 0, MEM_RELEASE);
    std::printf(failures ? "texture output: %d FAILED\n" : "texture output: all checks passed\n", failures);
    return failures ? 1 : 0;
}
