// Offscreen test of the command bar's pass: the analytical bar, and typed text in several scripts rasterised by the
// text thread and read by the shader, checked on the pixels. No video is involved.
#include "media/render/PalettePipeline.hpp"
#include "allocation_probe.hpp"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
using media::ui::CommandPalette; using media::ui::PaletteHint;
constexpr unsigned kMaxBytes = 1920 * 1080 * 4;
struct Image { std::byte pixels[kMaxBytes]; unsigned width = 0, height = 0;
    unsigned At(unsigned x, unsigned y, unsigned channel = 0) const { return static_cast<unsigned>(pixels[(std::size_t{y} * width + x) * 4 + channel]); }
};
Image a, b, c, blank; // Fixed storage: nothing is allocated while drawing.
struct Test {
    ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource *target = nullptr, *readback = nullptr; ID3D12DescriptorHeap* rtvs = nullptr; ID3D12InfoQueue* messages = nullptr;
    media::core::LinearMemoryArena arena; media::render::PalettePipeline pipeline; CommandPalette palette;
    HANDLE event = nullptr; std::byte* mapped = nullptr; std::uint64_t sequence = 0; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    unsigned width = 0, height = 0; std::int64_t now = 10000000;
    ~Test() {
        pipeline.Release(); Release(list); Release(allocator); Release(target);
        if (readback && mapped) readback->Unmap(0, nullptr);
        Release(readback); Release(rtvs); Release(fence); Release(queue); Release(messages); Release(device);
        if (event) CloseHandle(event);
    }
    HRESULT Prepare(unsigned w, unsigned h, float dpi) {
        width = w; height = h;
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); Release(debug); }
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr)) return hr;
        (void)device->QueryInterface(IID_PPV_ARGS(&messages));
        if (messages) { D3D12_MESSAGE_SEVERITY severities[]{D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR};
            D3D12_INFO_QUEUE_FILTER filter{}; filter.AllowList.NumSeverities = 2; filter.AllowList.pSeverityList = severities;
            (void)messages->PushStorageFilter(&filter); messages->ClearStoredMessages(); }
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, q.Type, allocator, nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        D3D12_DESCRIPTOR_HEAP_DESC rtv{}; rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv.NumDescriptors = 1;
        if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvs));
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = width; texture.Height = height; texture.MipLevels = texture.DepthOrArraySize = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texture.SampleDesc.Count = 1; texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target));
        if (FAILED(hr)) return hr;
        device->CreateRenderTargetView(target, nullptr, rtvs->GetCPUDescriptorHandleForHeapStart());
        UINT64 bytes = 0; device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes; buffer.Height = buffer.MipLevels = buffer.DepthOrArraySize = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; heap.Type = D3D12_HEAP_TYPE_READBACK;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        if (SUCCEEDED(hr)) hr = readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(hr) || !event) return E_FAIL;
        if (!arena.Initialize(4 * 1024 * 1024)) return E_OUTOFMEMORY;
        auto* storage = static_cast<char*>(arena.Allocate(CommandPalette::kStorageBytes));
        if (!storage) return E_OUTOFMEMORY;
        hr = pipeline.Prepare(device, arena);
        if (FAILED(hr)) return hr;
        if (!arena.Seal()) return E_FAIL; // Everything after this runs on sealed memory.
        palette.Configure({storage, CommandPalette::kStorageBytes}); palette.Layout(width, height, dpi);
        return S_OK;
    }
    // Opens the bar and lets it settle; the caret is in its steady phase.
    void Open() { (void)palette.Open(now); Step(40); }
    void Step(unsigned frames) { for (unsigned i = 0; i < frames; ++i) { now += 10000000 / 120; palette.Advance(now); } }
    void Type(std::u16string_view keys) { for (const char16_t key : keys) (void)palette.Utf16(static_cast<std::uint16_t>(key), now); palette.Advance(now); }
    bool Draw(Image& image, float background = 0.15f) {
        if (palette.Animating() && !pipeline.Settle(palette, 10000)) return false; // The text shown is the text typed.
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) return false;
        const float clear[]{background, background, background, 1}; const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        pipeline.Record(list, rtv, width, height, palette);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = target; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET; list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence, ++sequence)) || FAILED(fence->SetEventOnCompletion(sequence, event))) return false;
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return false;
        image.width = width; image.height = height;
        for (unsigned y = 0; y < height; ++y) std::memcpy(image.pixels + std::size_t{y} * width * 4, mapped + std::size_t{y} * footprint.Footprint.RowPitch, std::size_t{width} * 4);
        return true;
    }
};
unsigned Differences(const Image& x, const Image& y, unsigned left = 0, unsigned right = ~0u) {
    unsigned count = 0;
    for (unsigned row = 0; row < x.height; ++row)
        for (unsigned column = left; column < (std::min)(right, x.width); ++column)
            if (std::memcmp(x.pixels + (std::size_t{row} * x.width + column) * 4, y.pixels + (std::size_t{row} * x.width + column) * 4, 4) != 0) ++count;
    return count;
}
// Text pixels inside the lane: their number and horizontal extent, leaving out the caret's columns.
struct Ink { unsigned pixels = 0, left = ~0u, right = 0, top = ~0u, bottom = 0; };
Ink Measure(const Image& image, const media::ui::PaletteTextRect& lane, float caret, unsigned threshold = 150) {
    Ink ink; const int skip = static_cast<int>(std::round(caret));
    for (unsigned y = 0; y < lane.height; ++y)
        for (unsigned x = 0; x < lane.width; ++x) {
            if (static_cast<int>(x) >= skip - 1 && static_cast<int>(x) <= skip + 3) continue;
            if (image.At(static_cast<unsigned>(lane.x) + x, static_cast<unsigned>(lane.y) + y) < threshold) continue;
            ++ink.pixels; ink.left = (std::min)(ink.left, x); ink.right = (std::max)(ink.right, x); ink.top = (std::min)(ink.top, y); ink.bottom = (std::max)(ink.bottom, y);
        }
    return ink;
}
void Save(const char* directory, const char* name, const Image& image) {
    char path[1024]{}; std::snprintf(path, sizeof(path), "%s/%s.bmp", directory, name);
    FILE* file = nullptr; if (fopen_s(&file, path, "wb") || !file) { CHECK(false); return; }
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER); header.bfSize = header.bfOffBits + image.width * image.height * 4;
    BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = static_cast<LONG>(image.width); info.biHeight = -static_cast<LONG>(image.height); info.biPlanes = 1; info.biBitCount = 32;
    std::fwrite(&header, sizeof(header), 1, file); std::fwrite(&info, sizeof(info), 1, file);
    static std::byte row[1920 * 4];
    for (unsigned y = 0; y < image.height; ++y) {
        for (unsigned x = 0; x < image.width; ++x) {
            const auto source = (std::size_t{y} * image.width + x) * 4;
            row[x * 4] = image.pixels[source + 2]; row[x * 4 + 1] = image.pixels[source + 1]; row[x * 4 + 2] = image.pixels[source]; row[x * 4 + 3] = std::byte{255};
        }
        std::fwrite(row, image.width * 4, 1, file);
    }
    std::fclose(file);
}
}
int main(int argc, char** argv) {
    static Test t;
    const auto hr = t.Prepare(1280, 720, 1);
    if (FAILED(hr)) { std::printf("Palette GPU preparation failed %08lx\n", static_cast<unsigned long>(hr)); return 1; }
    const auto lane = t.palette.TextRect(); const auto& stats = t.pipeline.Stats();
    const auto u0 = t.palette.Uniforms();
    const unsigned cx = static_cast<unsigned>(u0.center_x), cy = static_cast<unsigned>(u0.center_y);
    // Printed before measuring: the first line also makes stdio allocate its buffer.
    std::printf("Bar %.0fx%.0f at (%u,%u), text lane %ux%u at (%d,%d), font %.0f px\n", static_cast<double>(2 * u0.half_width * u0.scale),
                static_cast<double>(2 * u0.half_height * u0.scale), cx, cy, lane.width, lane.height, lane.x, lane.y, static_cast<double>(lane.font_pixels));
    allocation_probe::Begin();
    // Closed: nothing is drawn.
    t.palette.Advance(t.now);
    CHECK(t.Draw(blank)); CHECK(blank.At(cx, cy) == 38 && blank.At(10, 10) == 38);
    // Open and settled, empty: the dark bar, its rim, the magnifier, and the caret at the lane's start.
    t.Open();
    CHECK(t.palette.Uniforms().visibility == 1 && t.palette.Uniforms().caret == 1);
    CHECK(t.Draw(a));
    const unsigned icon_x = static_cast<unsigned>(u0.center_x - u0.half_width + 28), top_rim = static_cast<unsigned>(u0.center_y - u0.half_height);
    CHECK(a.At(cx, cy + 20) < 30 && a.At(cx, top_rim) > a.At(cx, cy + 20) + 20); // Glass, and its lit upper rim.
    CHECK(a.At(10, 10) == 38 && a.At(cx, static_cast<unsigned>(u0.center_y + u0.half_height + 120)) == 38); // Nothing beyond the bar's shadow.
    CHECK(Differences(a, blank, icon_x - 12, icon_x + 12) > 40); // The magnifier.
    CHECK(stats.width == lane.width && stats.height == lane.height && stats.caret == 0 && !stats.right_to_left);
    CHECK(a.At(static_cast<unsigned>(lane.x), cy) > 150 && Measure(a, lane, 0).pixels == 0); // The caret, and no text.
    t.Step(80); CHECK(t.palette.Uniforms().caret == 0); // Two thirds of a second idle: the caret's dark phase.
    CHECK(t.Draw(b)); CHECK(b.At(static_cast<unsigned>(lane.x), cy) < 40 && Differences(a, b) > 20 && Differences(a, b) < 120);
    // Latin text: left-aligned, the caret after it.
    t.Type(u"pause"); CHECK(t.Draw(a));
    const auto latin = Measure(a, lane, stats.caret); const float latin_caret = stats.caret;
    std::printf("Latin \"pause\": %u ink pixels, columns %u-%u, rows %u-%u, caret at %.1f\n", latin.pixels, latin.left, latin.right, latin.top, latin.bottom, static_cast<double>(stats.caret));
    CHECK(latin.pixels > 150 && latin.left < 6 && latin.right > 30 && latin.right < 90 && !stats.right_to_left);
    CHECK(stats.caret >= static_cast<float>(latin.right) && stats.caret < static_cast<float>(latin.right) + 8);
    CHECK(latin.top > 8 && latin.bottom < lane.height - 8 && latin.bottom - latin.top >= 12); // Centred vertically, a 22 px font.
    CHECK(a.At(static_cast<unsigned>(lane.x) + static_cast<unsigned>(std::round(stats.caret)), cy) > 150); // The caret itself.
    // Typing on and deleting again returns to the very same image: text is rasterised, never accumulated.
    t.Type(u" now"); CHECK(t.Draw(b)); CHECK(Differences(a, b) > 80 && stats.caret > latin_caret + 20);
    for (unsigned i = 0; i < 4; ++i) (void)t.palette.Backspace(t.now);
    CHECK(t.palette.Text() == "pause" && t.Draw(b)); CHECK(Differences(a, b) == 0);
    if (argc > 1) Save(argv[1], "palette-latin", a);
    // Hebrew: right-to-left, flush against the lane's right end, the caret to the left of the text.
    t.Open(); t.Type(u"חזור 10 שניות"); CHECK(t.Draw(b));
    const auto hebrew = Measure(b, lane, stats.caret);
    std::printf("Hebrew: %u ink pixels, columns %u-%u, caret at %.1f, right-to-left %u\n", hebrew.pixels, hebrew.left, hebrew.right, static_cast<double>(stats.caret), stats.right_to_left ? 1u : 0u);
    CHECK(stats.right_to_left && hebrew.pixels > 250 && hebrew.right > lane.width - 8 && hebrew.left > lane.width / 2);
    CHECK(stats.caret <= static_cast<float>(hebrew.left) + 1 && stats.caret > static_cast<float>(hebrew.left) - 10);
    if (argc > 1) Save(argv[1], "palette-hebrew", b);
    // Arabic: right-to-left and joined (one connected word is wider than it is tall).
    t.Open(); t.Type(u"ابحث عن مرحبا"); CHECK(t.Draw(b));
    const auto arabic = Measure(b, lane, stats.caret);
    CHECK(stats.right_to_left && arabic.pixels > 250 && arabic.right > lane.width - 8 && stats.caret <= static_cast<float>(arabic.left) + 1);
    if (argc > 1) Save(argv[1], "palette-arabic", b);
    // Chinese and Japanese: fallback fonts supply the glyphs (dense ink, full-height characters).
    t.Open(); t.Type(u"快退10秒 一時停止"); CHECK(t.Draw(b));
    const auto cjk = Measure(b, lane, stats.caret);
    std::printf("CJK: %u ink pixels, columns %u-%u, rows %u-%u\n", cjk.pixels, cjk.left, cjk.right, cjk.top, cjk.bottom);
    CHECK(!stats.right_to_left && cjk.pixels > 600 && cjk.left < 6 && cjk.right > 150 && cjk.bottom - cjk.top >= 16);
    if (argc > 1) Save(argv[1], "palette-cjk", b);
    // Cyrillic, and accents.
    t.Open(); t.Type(u"перемотай назад — zurück, más"); CHECK(t.Draw(b));
    CHECK(Measure(b, lane, stats.caret).pixels > 600 && !stats.right_to_left);
    // A composition is underlined, the committed text is not.
    t.Open(); t.Type(u"find "); CHECK(t.Draw(a));
    t.palette.Compose(u"にほんご", 4, t.now); CHECK(t.Draw(b));
    const auto run = [&](const Image& image) { // Longest horizontal run of ink in the lane.
        unsigned longest = 0;
        for (unsigned y = 0; y < lane.height; ++y) {
            unsigned current = 0;
            for (unsigned x = 0; x < lane.width; ++x) {
                current = image.At(static_cast<unsigned>(lane.x) + x, static_cast<unsigned>(lane.y) + y) > 150 ? current + 1 : 0;
                longest = (std::max)(longest, current);
            }
        }
        return longest;
    };
    std::printf("Composition underline: longest ink run %u px (committed text alone: %u px)\n", run(b), run(a));
    CHECK(run(a) < 20 && run(b) > 60 && run(b) < 110);
    if (argc > 1) Save(argv[1], "palette-composition", b);
    (void)t.palette.Insert(u"日本語", 3, t.now); t.palette.Compose(u"", 0, t.now); CHECK(t.Draw(b)); CHECK(run(b) < 40 && t.palette.Text() == "find 日本語");
    // Text longer than the lane scrolls: the caret stays inside, the icon and the counter are never overdrawn.
    t.Open(); t.Type(u"a"); CHECK(t.Draw(a));
    for (unsigned i = 0; i < 12; ++i) t.Type(u"the quiet between the frames ");
    CHECK(t.Draw(b));
    const auto scrolled = Measure(b, lane, stats.caret);
    CHECK(stats.caret > static_cast<float>(lane.width) - 12 && stats.caret < static_cast<float>(lane.width) && scrolled.left < 8 && scrolled.pixels > 1500);
    CHECK(Differences(a, b, 0, static_cast<unsigned>(lane.x)) == 0 && Differences(a, b, static_cast<unsigned>(lane.x) + lane.width) == 0);
    t.Open(); for (unsigned i = 0; i < 12; ++i) t.Type(u"שלום עולם ");
    CHECK(t.Draw(b)); CHECK(stats.right_to_left && stats.caret < 12 && stats.caret >= 0 && Differences(a, b, 0, static_cast<unsigned>(lane.x)) == 0);
    // Hints: the chevron and the Return arrow of a command; the match counter of a search; amber for no match.
    const unsigned counter = static_cast<unsigned>(u0.center_x + u0.half_width - 34);
    t.Open(); t.Type(u"pause"); CHECK(t.Draw(a));
    t.palette.SetHint(PaletteHint::Command, 0); CHECK(t.Draw(b));
    CHECK(Differences(a, b, icon_x - 12, icon_x + 12) > 30 && Differences(a, b, counter - 14, counter + 14) > 20);
    t.palette.SetHint(PaletteHint::Search, 12); CHECK(t.Draw(b)); CHECK(Differences(a, b, counter - 16, counter + 18) > 30);
    t.palette.SetHint(PaletteHint::Search, 13); CHECK(t.Draw(c)); CHECK(Differences(b, c, counter - 16, counter + 18) > 4 && Differences(b, c, 0, counter - 16) == 0);
    if (argc > 1) Save(argv[1], "palette-search", c);
    t.palette.SetHint(PaletteHint::NoMatch, 0); CHECK(t.Draw(c));
    { unsigned warm = 0; for (unsigned x = counter; x < counter + 18; ++x) for (unsigned y = cy - 8; y < cy + 8; ++y) if (c.At(x, y, 0) > c.At(x, y, 2) + 40) ++warm; CHECK(warm > 8); }
    // A refusal tints the rim amber and moves the bar, text included, by whole pixels.
    t.palette.SetHint(PaletteHint::Empty, 0); CHECK(t.Draw(a));
    t.palette.Rejected(t.now); t.Step(1); CHECK(t.Draw(b));
    CHECK(b.At(cx, top_rim, 0) > b.At(cx, top_rim, 2) + 15 && a.At(cx, top_rim, 0) <= a.At(cx, top_rim, 2) + 4);
    if (argc > 1) Save(argv[1], "palette-refused", b);
    t.Step(300); CHECK(t.palette.Uniforms().denied == 0 && t.palette.Uniforms().shake == 0);
    // No seam: over white footage the shadow has decayed to the background at the scissor's edges.
    {
        using media::render::PalettePipeline;
        CHECK(t.Draw(b, 1.0f));
        const auto left = static_cast<unsigned>(u0.center_x - (u0.half_width + PalettePipeline::kMargin) * u0.scale);
        const auto right = static_cast<unsigned>(u0.center_x + (u0.half_width + PalettePipeline::kMargin) * u0.scale) - 1;
        const auto top = static_cast<unsigned>(u0.center_y - (u0.half_height + PalettePipeline::kMargin) * u0.scale);
        const auto bottom = static_cast<unsigned>(u0.center_y + (u0.half_height + PalettePipeline::kMargin) * u0.scale) - 1;
        std::printf("Bar on white: edges %u %u %u %u, shadow below the bar %u, glass %u\n", b.At(left, cy), b.At(right, cy), b.At(cx, top), b.At(cx, bottom),
                    b.At(cx, static_cast<unsigned>(u0.center_y + u0.half_height + 12)), b.At(cx, cy + 20));
        CHECK(b.At(left, cy) == 255 && b.At(right, cy) == 255 && b.At(cx, top) == 255 && b.At(cx, bottom) == 255);
        CHECK(b.At(cx, static_cast<unsigned>(u0.center_y + u0.half_height + 12)) < 235 && b.At(cx, cy + 20) < 60); // The shadow exists; the glass stays dark.
        if (argc > 1) Save(argv[1], "palette-on-white", b);
    }
    // Dismissal: the whole bar fades and lifts; halfway it is between the two, at the end nothing is left.
    CHECK(t.Draw(a));
    t.palette.Close(t.now);
    bool fading = false;
    for (unsigned i = 0; i < 40 && t.palette.Visible(); ++i) {
        t.Step(1); CHECK(t.Draw(b));
        const float v = t.palette.Uniforms().visibility;
        if (v > 0.3f && v < 0.7f) { fading = true; CHECK(b.At(cx, cy + 4) > a.At(cx, cy + 4) && b.At(cx, cy + 4) < blank.At(cx, cy + 4)); }
    }
    CHECK(fading && !t.palette.Visible()); CHECK(t.Draw(b)); CHECK(Differences(b, blank) == 0);
    // Keystroke to raster: the text thread's turnaround for one more character (no GPU involved).
    t.Open(); t.Type(u"find the quiet"); CHECK(t.pipeline.Settle(t.palette, 5000));
    LARGE_INTEGER frequency{}, begin{}, end{}; QueryPerformanceFrequency(&frequency);
    double slowest = 0, total = 0; constexpr unsigned kKeys = 40;
    for (unsigned i = 0; i < kKeys; ++i) {
        QueryPerformanceCounter(&begin);
        (void)t.palette.Utf16(static_cast<std::uint16_t>(i % 3 == 0 ? 0x65e5 : i % 3 == 1 ? 0x05e9 : 'e'), t.now);
        while (!t.pipeline.Settle(t.palette, 0)) YieldProcessor();
        QueryPerformanceCounter(&end);
        const double ms = static_cast<double>(end.QuadPart - begin.QuadPart) * 1000 / static_cast<double>(frequency.QuadPart);
        slowest = (std::max)(slowest, ms); total += ms;
    }
    // A device recovery: the pass is rebuilt and the same text comes back identically.
    t.Open(); t.Type(u"go to 1:30"); CHECK(t.Draw(a));
    CHECK(SUCCEEDED(t.pipeline.Recreate(t.device))); CHECK(t.Draw(b)); CHECK(Differences(a, b) == 0);
    const auto allocations = allocation_probe::End(); CHECK(allocations == 0);
    if (t.messages) CHECK(t.messages->GetNumStoredMessages() == 0);
    CHECK(stats.failures == 0 && stats.rasters == stats.requests && stats.uploads > 10);
    std::printf("Text thread: %u rasters, %u uploads, %u failures; keystroke to raster %.2f ms mean, %.2f ms slowest\n", stats.rasters, stats.uploads, stats.failures, total / kKeys, slowest);
    // A larger, denser window: the text is rasterised at the scaled size, not stretched.
    static Test dense;
    CHECK(SUCCEEDED(dense.Prepare(1920, 1080, 1.5f)));
    if (!failures) {
        const auto wide = dense.palette.TextRect();
        dense.Open(); dense.Type(u"pause"); CHECK(dense.Draw(c));
        const auto scaled = Measure(c, wide, dense.pipeline.Stats().caret);
        std::printf("At 150 %%: lane %ux%u, font %.0f px, \"pause\" spans %u px (at 100 %%: %u px)\n", wide.width, wide.height, static_cast<double>(wide.font_pixels),
                    scaled.right - scaled.left, latin.right - latin.left);
        CHECK(dense.palette.Uniforms().scale == 1.5f && wide.width == 786 && wide.font_pixels == 33);
        const float ratio = static_cast<float>(scaled.right - scaled.left) / static_cast<float>(latin.right - latin.left);
        CHECK(ratio > 1.35f && ratio < 1.65f && dense.pipeline.Stats().failures == 0);
        if (argc > 1) Save(argv[1], "palette-150", c);
        if (dense.messages) CHECK(dense.messages->GetNumStoredMessages() == 0);
    }
    std::printf("Command bar GPU: %d failures, %llu allocations\n", failures, static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
