// Multi-resolution presentation test: the production dock and caption passes composited offscreen at window sizes
// from 640x360 to 4K, landscape, ultrawide and portrait, at several DPI scales. Measured from pixels: the dock keeps
// its capsule proportions, centring and bottom margin; captions keep their proportions relative to the font size,
// stay centred inside the frame and clear the dock. Only synthetic UI output is read back.
#include "media/render/D3D12Renderer.hpp"
#include "allocation_probe.hpp"
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
constexpr unsigned MaxWidth = 3840, MaxHeight = 3840; // Covers 4K landscape and portrait.
unsigned char pixels[MaxWidth * MaxHeight]{};         // Red channel of the last draw (fixed storage).
struct Gpu {
    ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource *target = nullptr, *readback = nullptr; ID3D12DescriptorHeap* rtvs = nullptr; ID3D12InfoQueue* messages = nullptr;
    HANDLE event = nullptr; std::byte* mapped = nullptr; std::uint64_t sequence = 0; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    media::core::LinearMemoryArena arena; media::subtitle::SubtitleTrack track; media::render::SdfSubtitlePipeline subtitles;
    media::render::HudPipeline hud;
    ~Gpu() {
        subtitles.Release(); hud.Release(); Release(list); Release(allocator); Release(target);
        if (readback && mapped) readback->Unmap(0, nullptr);
        Release(readback); Release(rtvs); Release(fence); Release(queue); Release(messages); Release(device);
        if (event) CloseHandle(event);
    }
    HRESULT Prepare() {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); Release(debug); }
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)); if (FAILED(hr)) return hr;
        (void)device->QueryInterface(IID_PPV_ARGS(&messages));
        if (messages) {
            D3D12_MESSAGE_SEVERITY severities[]{D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR};
            D3D12_INFO_QUEUE_FILTER filter{}; filter.AllowList.NumSeverities = 2; filter.AllowList.pSeverityList = severities;
            (void)messages->PushStorageFilter(&filter); messages->ClearStoredMessages();
        }
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, q.Type, allocator, nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr)) hr = hud.Create(device);
        D3D12_DESCRIPTOR_HEAP_DESC rtv{}; rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv.NumDescriptors = 1;
        if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvs));
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = MaxWidth; texture.Height = MaxHeight;
        texture.MipLevels = texture.DepthOrArraySize = 1; texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texture.SampleDesc.Count = 1;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target));
        if (FAILED(hr)) return hr;
        device->CreateRenderTargetView(target, nullptr, rtvs->GetCPUDescriptorHandleForHeapStart());
        UINT64 bytes = 0; device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes;
        buffer.Height = buffer.MipLevels = buffer.DepthOrArraySize = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        if (SUCCEEDED(hr)) hr = readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(hr) || !event) return E_FAIL;
        // Uppercase without descenders: the caption's ink box is its cap height times its advance width.
        if (!arena.Initialize(48 * 1024 * 1024) || !track.Prepare(arena, "WEBVTT\n\n00:00.000 --> 00:10.000\nHIGH NOTE\n")) return E_FAIL;
        hr = subtitles.Prepare(device, queue, arena, track);
        if (SUCCEEDED(hr) && (!arena.Seal() || !subtitles.WaitShaped(10000))) return E_FAIL; // Shaping runs on its own thread.
        return hr;
    }
    // Mid-grey frame, then captions at 5 s and/or the dock, in the renderer's order and with its caption inset. They are
    // measured in separate passes: the captions' dark outline would otherwise read as dock glass.
    bool Draw(unsigned width, unsigned height, float dpi, const media::ui::HudUniforms& u, bool captions, bool dock) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) return false;
        constexpr float grey[]{0.25f, 0.25f, 0.25f, 1}; const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        list->ClearRenderTargetView(rtv, grey, 0, nullptr);
        if (captions && subtitles.Settle(50000000, 5000)) (void)subtitles.Record(list, rtv, width, height, 50000000, media::render::D3D12Renderer::CaptionInset(u, width, height, dpi), dpi);
        if (dock) hud.Record(list, rtv, width, height, u);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = target; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
        const D3D12_BOX box{0, 0, 0, width, height, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence, ++sequence)) || FAILED(fence->SetEventOnCompletion(sequence, event))) return false;
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return false;
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) pixels[y * width + x] = static_cast<unsigned char>(mapped[y * footprint.Footprint.RowPitch + x * 4]);
        return true;
    }
};
struct Box { int left = -1, top = -1, right = -1, bottom = -1; };
// Bounding box of pixels matching `test` inside rows [row0, row1).
template<class Test> Box Bounds(unsigned width, unsigned row0, unsigned row1, Test test) {
    Box b{};
    for (unsigned y = row0; y < row1; ++y)
        for (unsigned x = 0; x < width; ++x)
            if (test(pixels[y * width + x])) {
                if (b.left < 0 || static_cast<int>(x) < b.left) b.left = static_cast<int>(x);
                if (static_cast<int>(x) > b.right) b.right = static_cast<int>(x);
                if (b.top < 0) b.top = static_cast<int>(y);
                b.bottom = static_cast<int>(y);
            }
    return b;
}
}
int main(int argc, char** argv) {
    Gpu gpu; const HRESULT hr = gpu.Prepare();
    if (FAILED(hr)) { std::printf("Layout GPU preparation failed %08lx\n", static_cast<unsigned long>(hr)); return 1; }
    struct Size { unsigned w, h; float dpi; const char* name; };
    constexpr Size sizes[]{{1280, 720, 1, "720p"}, {1920, 1080, 1, "1080p"}, {2560, 1440, 1.25f, "1440p@125%"}, {3840, 2160, 1, "4K@100%"},
                           {3840, 2160, 1.5f, "4K@150%"}, {2560, 1080, 1, "21:9"}, {3440, 1440, 1, "21:9 1440"}, {1080, 1920, 1, "9:16"},
                           {2160, 3840, 1.5f, "9:16 4K@150%"}, {720, 1280, 2, "9:16@200%"}, {640, 360, 1, "360p"}};
    // Measured first, judged afterwards against the 4K@100% captions (the largest, least quantised measurement).
    struct Measure { float font, text_w, text_h, gap, scale, offset; bool dock_ok, margins, inside; float dock_w, dock_h; int dock_x, dock_y; };
    Measure m[std::size(sizes)]{};
    std::printf("Layout at %zu sizes (dock and captions measured from pixels):\n", std::size(sizes)); // Primes stdout's buffer.
    allocation_probe::Begin();
    for (unsigned i = 0; i < std::size(sizes); ++i) {
        const auto& z = sizes[i];
        media::ui::KineticHud model; model.Configure(z.w, z.h, 36000000000ll, true, z.dpi);
        auto u = model.Uniforms(); u.freeze = 1; u.energy = 0; u.progress = 0.4f; u.elapsed_seconds = 1440; u.duration_seconds = 3600;
        if (!gpu.Draw(z.w, z.h, z.dpi, u, false, true)) { CHECK(false); break; }
        // Dock: the dark glass (the grey background is 64/255; the glass stays below 30 over it). Its rim highlight is
        // brighter, so the measured box sits up to ~1.3 design px inside the geometric edge: tolerance 2 scaled px.
        const Box dock = Bounds(z.w, 0, z.h, [](unsigned char v) { return v < 30; });
        if (!gpu.Draw(z.w, z.h, z.dpi, u, true, false)) { CHECK(false); break; }
        const Box text = Bounds(z.w, 0, z.h, [](unsigned char v) { return v > 200; });
        const float s = u.scale, half_w = u.dock_half_width * s, half_h = u.dock_half_height * s, tol = (std::max)(2.0f, 2 * s);
        auto& r = m[i];
        r.scale = s; r.font = media::render::SdfSubtitlePipeline::FontPixels(z.w, z.h, z.dpi);
        r.dock_w = static_cast<float>(dock.right - dock.left + 1); r.dock_h = static_cast<float>(dock.bottom - dock.top + 1); r.dock_x = dock.left; r.dock_y = dock.top;
        r.dock_ok = std::fabs(dock.left - (u.center_x - half_w)) <= tol && std::fabs(dock.right + 1 - (u.center_x + half_w)) <= tol &&
                    std::fabs(dock.top - (u.center_y - half_h)) <= tol && std::fabs(dock.bottom + 1 - (u.center_y + half_h)) <= tol &&
                    (u.dock_half_width != 280 || std::fabs(r.dock_w / r.dock_h / (280.0f / 52) - 1) < 0.03f);
        r.margins = std::fabs((static_cast<float>(z.h) - static_cast<float>(dock.bottom + 1)) - 26 * s) <= tol &&
                    std::abs((dock.left + dock.right + 1) - static_cast<int>(z.w)) <= 2 && dock.left >= static_cast<int>(16 * s - tol);
        r.text_w = static_cast<float>(text.right - text.left + 1); r.text_h = static_cast<float>(text.bottom - text.top + 1);
        r.gap = static_cast<float>(dock.top - text.bottom - 1);
        // Inside the frame, centred, and at least 20 scaled px (less one pixel of antialiasing) above the dock. The ink box
        // is off-centre by the font's unequal side bearings, which scale with the font: 10 % of its size (min 3 px).
        r.offset = static_cast<float>(text.left + text.right + 1 - static_cast<int>(z.w)) * 0.5f;
        r.inside = text.top > 0 && text.left > 0 && text.right < static_cast<int>(z.w) - 1 &&
                   std::fabs(r.offset) <= (std::max)(3.0f, 0.1f * r.font) && r.gap >= 20 * s - 1;
    }
    const auto allocations = allocation_probe::End();
    const auto& ref = m[3];
    for (unsigned i = 0; i < std::size(sizes); ++i) {
        const auto& z = sizes[i]; const auto& r = m[i];
        // Uniform scaling: caption width and height follow the font size (within 2 px of quantisation plus 3 %).
        const float k = r.font / ref.font;
        const bool proportional = std::fabs(r.text_w - ref.text_w * k) <= 2 + 0.03f * r.text_w && std::fabs(r.text_h - ref.text_h * k) <= 2 + 0.03f * r.text_h;
        const bool ok = r.dock_ok && r.margins && r.inside && proportional;
        std::printf("%-13s %4ux%-4u dpi %.2f: dock %4.0fx%-3.0f at (%d,%d) aspect %.3f scale %.3f | captions %4.0fx%-3.0f font %4.1f px, %4.1f%% of height, off-centre %+.1f px, gap %3.0f px %s\n",
                    z.name, z.w, z.h, static_cast<double>(z.dpi), static_cast<double>(r.dock_w), static_cast<double>(r.dock_h), r.dock_x, r.dock_y,
                    static_cast<double>(r.dock_w / r.dock_h), static_cast<double>(r.scale), static_cast<double>(r.text_w), static_cast<double>(r.text_h),
                    static_cast<double>(r.font), static_cast<double>(100 * r.font / static_cast<float>(z.h)), static_cast<double>(r.offset), static_cast<double>(r.gap), ok ? "ok" : "WRONG");
        CHECK(r.dock_ok); CHECK(r.margins); CHECK(r.inside); CHECK(proportional);
    }
    CHECK(allocations == 0);
    // Optional review images (greyscale, red channel): captions and dock composited together.
    for (unsigned i = 0; argc > 1 && i < std::size(sizes); ++i) {
        const auto& z = sizes[i];
        media::ui::KineticHud model; model.Configure(z.w, z.h, 36000000000ll, true, z.dpi);
        auto u = model.Uniforms(); u.freeze = 1; u.energy = 0; u.progress = 0.4f; u.elapsed_seconds = 1440; u.duration_seconds = 3600;
        if (!gpu.Draw(z.w, z.h, z.dpi, u, true, true)) break;
        char path[512]{}; std::snprintf(path, sizeof(path), "%s/layout-%ux%u-%03u.bmp", argv[1], z.w, z.h, static_cast<unsigned>(z.dpi * 100));
        FILE* file = nullptr; if (fopen_s(&file, path, "wb") || !file) break;
        const unsigned row = (z.w * 3 + 3) & ~3u;
        BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER); header.bfSize = header.bfOffBits + row * z.h;
        BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = static_cast<LONG>(z.w); info.biHeight = -static_cast<LONG>(z.h); info.biPlanes = 1; info.biBitCount = 24;
        std::fwrite(&header, sizeof(header), 1, file); std::fwrite(&info, sizeof(info), 1, file);
        static unsigned char line[MaxWidth * 3 + 4];
        for (unsigned y = 0; y < z.h; ++y) {
            for (unsigned x = 0; x < z.w; ++x) line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = pixels[y * z.w + x];
            std::fwrite(line, row, 1, file);
        }
        std::fclose(file);
    }
    if (gpu.messages) CHECK(gpu.messages->GetNumStoredMessages() == 0);
    std::printf("Layout GPU: %d failures, %llu allocations\n", failures, static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
