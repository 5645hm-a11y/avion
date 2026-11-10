// The HUD's status cue (docs/kinetic-hud.md): the portable model (text, layout, the closed-form fade and width glide,
// track labels) and, on Windows, the GPU pass rendered offscreen and checked on its pixels (glass, icon, text raster in
// Latin, Hebrew and mixed scripts, the warning tint, nothing drawn while hidden). No allocation after setup.
#include "media/ui/StatusCue.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#if defined(_WIN32)
#include "media/render/StatusPipeline.hpp"
#endif
using media::ui::StatusCue; using media::ui::StatusIcon; using media::ui::StatusTone;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr std::int64_t S = 10000000;
bool Near(float a, float b, float tolerance = 1e-3f) { return std::abs(a - b) <= tolerance; }
bool WellFormed(std::string_view text) {
    while (!text.empty()) {
        const auto lead = static_cast<unsigned char>(text.front());
        const std::size_t n = lead < 0x80 ? 1 : lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 0;
        if (!n || n > text.size()) return false;
        for (std::size_t i = 1; i < n; ++i) if ((static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) return false;
        text.remove_prefix(n);
    }
    return true;
}
void Model() {
    StatusCue cue; cue.Layout(1280, 720, 1);
    const auto& u = cue.Uniforms();
    CHECK(u.scale == 1 && u.center_x == 640 && u.center_y == StatusCue::kTop + StatusCue::kHalfHeight);
    const auto lane = cue.TextRect();
    CHECK(lane.height == 38 && lane.font_pixels == StatusCue::kFontPixels && lane.width == static_cast<unsigned>(2 * StatusCue::kMaxHalfWidth - StatusCue::kIconWidth - 2 * StatusCue::kPadding));
    allocation_probe::Begin();
    std::int64_t t0 = 100 * S;
    cue.Show("Audio \xc2\xb7 Hebrew (\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa) \xc2\xb7 AAC stereo", StatusIcon::Audio, StatusTone::Info, t0);
    cue.Advance(t0 + S / 10);
    // Without its raster, nothing is shown: a cue is seen whole or not yet.
    CHECK(cue.Animating() && !cue.Visible() && cue.ShownRevision() == 0 && u.visibility == 0);
    const auto first = cue.Revision();
    cue.Ready(first + 7, 100, t0); // Another revision's raster: ignored.
    CHECK(cue.ShownRevision() == 0);
    cue.Ready(first, 200, t0);
    CHECK(cue.ShownRevision() == first && Near(u.half_width, 0) == false);
    cue.Advance(t0);
    CHECK(u.visibility == 0 && cue.Animating() && cue.Fading() && Near(u.half_width, (2 * StatusCue::kPadding + StatusCue::kIconWidth + 200) / 2));
    CHECK(Near(u.icon, static_cast<float>(StatusIcon::Audio)) && u.tone == 0 && Near(u.travel, StatusCue::kTravel));
    // The fade in is monotonic and ease-out cubic: 0.875 at half its 0.22 s, whole at the end; travel follows.
    float previous = -1; bool monotonic = true;
    for (std::int64_t ms = 0; ms <= 220; ++ms) { cue.Advance(t0 + ms * 10000); monotonic = monotonic && u.visibility >= previous; previous = u.visibility; }
    CHECK(monotonic && u.visibility == 1 && u.travel == 0);
    cue.Advance(t0 + 1100000); CHECK(Near(u.visibility, 0.875f, 2e-3f) && Near(u.travel, 0.125f * StatusCue::kTravel, 0.02f));
    // Held for 2.5 s, out over 0.5 s (ease-in cubic: 0.875 half way out), gone after 3.22 s.
    cue.Advance(t0 + 2 * S); CHECK(u.visibility == 1 && !cue.Fading());
    cue.Advance(t0 + 27200000 + 2500000); CHECK(Near(u.visibility, 1 - 0.125f, 2e-3f) && cue.Fading());
    cue.Advance(t0 + 32300000); CHECK(u.visibility == 0 && !cue.Visible() && !cue.Animating());
    // A new text while the pill is up keeps it up, restarts the hold, and the width glides over 0.15 s.
    std::int64_t t1 = 200 * S;
    cue.Show("Subtitles \xc2\xb7 English", StatusIcon::Subtitles, StatusTone::Info, t1); cue.Ready(cue.Revision(), 100, t1);
    for (std::int64_t ms = 0; ms <= 1000; ms += 10) cue.Advance(t1 + ms * 10000);
    const float narrow = u.half_width;
    const auto t2 = t1 + S;
    cue.Show("Audio \xc2\xb7 Commentary \xc2\xb7 English \xc2\xb7 AAC 5.1", StatusIcon::Audio, StatusTone::Info, t2);
    cue.Advance(t2 + S / 50); CHECK(u.visibility == 1 && Near(u.icon, static_cast<float>(StatusIcon::Subtitles))); // The old one, until its raster.
    cue.Ready(cue.Revision(), 300, t2 + S / 20);
    cue.Advance(t2 + S / 20); CHECK(u.visibility == 1 && Near(u.half_width, narrow) && Near(u.icon, static_cast<float>(StatusIcon::Audio)));
    const float wide = (2 * StatusCue::kPadding + StatusCue::kIconWidth + 300) / 2;
    cue.Advance(t2 + S / 20 + 750000); CHECK(Near(u.half_width, narrow + (wide - narrow) * 0.875f, 0.05f));
    cue.Advance(t2 + S / 20 + 1500000); CHECK(Near(u.half_width, wide));
    cue.Advance(t2 + S / 20 + 24000000); CHECK(u.visibility == 1); // The hold (2.5 s) started again at the new raster.
    cue.Advance(t2 + S / 20 + 33000000); CHECK(u.visibility == 0);
    // Empty text takes it down at once (as a fade).
    const auto t3 = 300 * S;
    cue.Show("Device", StatusIcon::Device, StatusTone::Info, t3); cue.Ready(cue.Revision(), 80, t3); cue.Advance(t3 + S);
    CHECK(u.visibility == 1);
    cue.Show("", StatusIcon::None, StatusTone::Info, t3 + S); cue.Advance(t3 + S + 2500000); CHECK(Near(u.visibility, 1 - 0.125f, 2e-3f));
    cue.Advance(t3 + S + 5100000); CHECK(u.visibility == 0);
    // Widths stay inside the pill's limits; the warning tone is carried.
    cue.Show("x", StatusIcon::Warning, StatusTone::Warning, 400 * S); cue.Ready(cue.Revision(), 5000, 400 * S); cue.Advance(400 * S + S);
    CHECK(u.half_width == StatusCue::kMaxHalfWidth && u.tone == 1 && Near(u.icon, static_cast<float>(StatusIcon::Warning)));
    cue.Show("y", StatusIcon::None, StatusTone::Info, 500 * S); cue.Ready(cue.Revision(), 0, 500 * S + S); cue.Advance(500 * S + 2 * S);
    CHECK(Near(u.half_width, (std::max)(StatusCue::kMinHalfWidth, (2 * StatusCue::kPadding + StatusCue::kIconWidth) / 2)));
    // Text: cut before a character that does not fit; controls become spaces, ill-formed bytes question marks.
    char long_text[400]; for (unsigned i = 0; i < 199; ++i) { long_text[2 * i] = '\xc3'; long_text[2 * i + 1] = '\xa9'; }
    cue.Show({long_text, 398}, StatusIcon::None, StatusTone::Info, 600 * S);
    CHECK(cue.Text().size() == StatusCue::kMaxBytes && WellFormed(cue.Text()));
    cue.Show("\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa", StatusIcon::None, StatusTone::Info, 600 * S); // Exactly fitting text stays whole.
    CHECK(cue.Text() == "\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa");
    cue.Show("a\tb\nc\xff" "d", StatusIcon::None, StatusTone::Info, 600 * S); CHECK(cue.Text() == "a b\nc?d" && cue.Lines() == 2);
    // Lines (the telemetry panel): a line feed starts a line, at most kMaxLines; an empty line or a last line feed is not
    // one. The panel takes its height when its text appears, its top edge staying where the pill's is.
    {
        StatusCue panel; panel.Layout(1280, 720, 1);
        const auto& pu = panel.Uniforms();
        panel.Show("one", StatusIcon::None, StatusTone::Info, S); panel.Ready(panel.Revision(), 30, S); panel.Advance(2 * S);
        CHECK(panel.ShownLines() == 1 && pu.half_height == StatusCue::kHalfHeight);
        panel.Show("1\n2\n\n3\n", StatusIcon::Device, StatusTone::Info, 2 * S);
        CHECK(panel.Text() == "1\n2\n3" && panel.Lines() == 3);
        const auto pending = panel.PendingRect(), shown = panel.TextRect();
        CHECK(pending.height == static_cast<unsigned>(2 * (StatusCue::kHalfHeight + StatusCue::kLineHeight)) && shown.height == 2 * StatusCue::kHalfHeight);
        CHECK(pending.y == shown.y && pending.width > shown.width); // Same top; a panel's lane is wider than the pill's.
        CHECK(pu.half_height == StatusCue::kHalfHeight); // Not before its text is there.
        panel.Ready(panel.Revision(), 500, 2 * S); panel.Advance(3 * S);
        CHECK(panel.ShownLines() == 3 && pu.half_height == StatusCue::kHalfHeight + StatusCue::kLineHeight);
        CHECK(pu.center_y == StatusCue::kTop + pu.half_height && pu.half_width > StatusCue::kMaxHalfWidth - 100);
        char many[64]; unsigned n = 0;
        for (unsigned line = 0; line < 10; ++line) { if (line) many[n++] = '\n'; many[n++] = static_cast<char>('a' + line); }
        panel.Show({many, n}, StatusIcon::None, StatusTone::Info, 4 * S);
        CHECK(panel.Lines() == StatusCue::kMaxLines && panel.Text() == "a\nb\nc\nd\ne\nf\ng\nh i j");
        panel.Ready(panel.Revision(), 5000, 4 * S); panel.Advance(5 * S);
        CHECK(pu.half_width <= StatusCue::kMaxPanelHalfWidth && pu.half_width > StatusCue::kMaxHalfWidth);
        StatusCue slim; slim.Layout(640, 360, 1); // A panel stays inside a narrow window.
        slim.Show("x\ny", StatusIcon::None, StatusTone::Info, S); slim.Ready(slim.Revision(), 5000, S); slim.Advance(2 * S);
        CHECK(slim.Uniforms().half_width * slim.Uniforms().scale <= 320);
    }
    // Reduced motion: no slide, 0.1 s fades.
    StatusCue still; still.Layout(1920, 1080, 1.5f); still.SetReducedMotion(true);
    still.Show("Audio", StatusIcon::Audio, StatusTone::Info, S); still.Ready(still.Revision(), 50, S);
    still.Advance(S + 500000); CHECK(Near(still.Uniforms().visibility, 0.875f, 2e-3f) && still.Uniforms().travel == 0);
    still.Advance(S + 1000000); CHECK(still.Uniforms().visibility == 1);
    CHECK(still.Uniforms().scale > 1 && still.TextRect().font_pixels > StatusCue::kFontPixels); // Follows the dock's scale.
    // Track labels.
    char label[StatusCue::kMaxBytes];
    media::ui::TrackLabel(label, sizeof(label), true, "", "Hebrew", "\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa", "heb", 2, 3);
    CHECK(std::string_view{label} == "Audio \xc2\xb7 Hebrew (\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa) \xc2\xb7 AAC stereo");
    media::ui::TrackLabel(label, sizeof(label), false, "", "English", "English", "eng", 0, 7);
    CHECK(std::string_view{label} == "Subtitles \xc2\xb7 English");
    media::ui::TrackLabel(label, sizeof(label), true, "Commentary", nullptr, nullptr, "kor", 6, 4);
    CHECK(std::string_view{label} == "Audio \xc2\xb7 Commentary \xc2\xb7 kor \xc2\xb7 AAC 5.1");
    media::ui::TrackLabel(label, sizeof(label), false, "", nullptr, nullptr, "", 0, 9);
    CHECK(std::string_view{label} == "Subtitles \xc2\xb7 Track 9");
    char tiny[12]; const auto n = media::ui::TrackLabel(tiny, sizeof(tiny), true, "", "Hebrew", nullptr, "heb", 1, 3);
    CHECK(n < sizeof(tiny) && std::string_view{tiny} == "Audio" && WellFormed(tiny)); // Pieces that do not fit are left out whole.
    // Quotes of a cue: as displayed (tags, entities, line breaks), cut at a word near the limit with an ellipsis.
    using media::ui::QuoteExcerpt;
    CHECK(QuoteExcerpt("<i>Be careful</i> &amp; run!", label, sizeof(label), 28) == 19 && std::string_view{label} == "\"Be careful & run!\"");
    // The film's cue: 28 letters end inside "המציקים"; the last space within the final quarter is before it.
    CHECK(QuoteExcerpt("\xd7\x96\xd7\x94\xd7\x99\xd7\xa8\xd7\x95\xd7\xaa! \xd7\xa9\xd7\x9c\xd7\x95\xd7\xa9\xd7\xaa \xd7\x94\xd7\xa1\xd7\xa0\xd7\x90\xd7\x99\xd7\x9d "
                       "\xd7\x94\xd7\x9e\xd7\xa6\xd7\x99\xd7\xa7\xd7\x99\xd7\x9d \xd7\x9e\xd7\x92\xd7\x99\xd7\xa2\xd7\x99\xd7\x9d!\n\xd8\xa7\xd9\x86\xd8\xaa\xd8\xa8\xd9\x87!",
                       label, sizeof(label), 28) > 0);
    CHECK(std::string_view{label} == "\"\xd7\x96\xd7\x94\xd7\x99\xd7\xa8\xd7\x95\xd7\xaa! \xd7\xa9\xd7\x9c\xd7\x95\xd7\xa9\xd7\xaa \xd7\x94\xd7\xa1\xd7\xa0\xd7\x90\xd7\x99\xd7\x9d"
                                     " \xd7\x94\xd7\x9e\xd7\xa6\xd7\x99\xd7\xa7\xd7\x99\xd7\x9d\xe2\x80\xa6\"" && WellFormed(label));
    CHECK(QuoteExcerpt("abcdefghijklmnopqrstuvwxyz0123456789", label, sizeof(label), 10) == 15 && std::string_view{label} == "\"abcdefghij\xe2\x80\xa6\""); // No space: cut.
    CHECK(QuoteExcerpt("  \n ", label, sizeof(label), 28) == 0 && QuoteExcerpt("\xff", label, sizeof(label), 28) == 0);
    CHECK(QuoteExcerpt("Be careful", tiny, 6, 28) == 0 && tiny[0] == 0); // Does not fit: nothing.
    CHECK(allocation_probe::End() == 0);
}
#if defined(_WIN32)
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
constexpr unsigned kWidth = 1280, kHeight = 720;
struct Image { std::byte pixels[kWidth * kHeight * 4]; unsigned At(unsigned x, unsigned y, unsigned c = 0) const { return static_cast<unsigned>(pixels[(std::size_t{y} * kWidth + x) * 4 + c]); } };
Image shown, hidden, warned, hebrew;
struct Gpu {
    ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource *target = nullptr, *readback = nullptr; ID3D12DescriptorHeap* rtvs = nullptr;
    media::core::LinearMemoryArena arena; media::render::StatusPipeline pipeline; StatusCue cue;
    HANDLE event = nullptr; std::byte* mapped = nullptr; std::uint64_t sequence = 0; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ~Gpu() {
        pipeline.Release(); Release(list); Release(allocator); Release(target);
        if (readback && mapped) readback->Unmap(0, nullptr);
        Release(readback); Release(rtvs); Release(fence); Release(queue); Release(device);
        if (event) CloseHandle(event);
    }
    HRESULT Prepare() {
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr)) return hr;
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, q.Type, allocator, nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        D3D12_DESCRIPTOR_HEAP_DESC rtv{}; rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv.NumDescriptors = 1;
        if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvs));
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = kWidth; texture.Height = kHeight;
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
        if (FAILED(hr) || !event || !arena.Initialize(4 * 1024 * 1024)) return E_FAIL;
        hr = pipeline.Prepare(device, arena);
        if (FAILED(hr) || !arena.Seal()) return FAILED(hr) ? hr : E_FAIL;
        cue.Layout(kWidth, kHeight, 1);
        return S_OK;
    }
    // Shows `text` and lets the pill come fully up (its raster made), then draws it over a mid-grey picture.
    bool Show(std::string_view text, StatusIcon icon, StatusTone tone, Image& image, std::int64_t& now) {
        cue.Show(text, icon, tone, now);
        if (!pipeline.Settle(cue, now, 10000)) return false;
        now += S; cue.Advance(now); // Held.
        return Draw(image);
    }
    bool Draw(Image& image) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) return false;
        const float grey[]{0.5f, 0.5f, 0.5f, 1}; const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        list->ClearRenderTargetView(rtv, grey, 0, nullptr);
        if (cue.Visible()) pipeline.Record(list, rtv, kWidth, kHeight, cue);
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
        for (unsigned y = 0; y < kHeight; ++y) std::memcpy(image.pixels + std::size_t{y} * kWidth * 4, mapped + std::size_t{y} * footprint.Footprint.RowPitch, std::size_t{kWidth} * 4);
        return true;
    }
};
// White text pixels inside the pill; dark glass pixels; pixels changed outside its box.
struct Look { unsigned ink = 0, glass = 0, outside = 0, amber = 0; unsigned ink_left = ~0u, ink_right = 0; };
Look Inspect(const Image& image, const StatusCue& cue) {
    Look look; const auto& u = cue.Uniforms();
    const float left = u.center_x - u.half_width * u.scale, right = u.center_x + u.half_width * u.scale;
    const float top = u.center_y - u.half_height * u.scale, bottom = u.center_y + u.half_height * u.scale;
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x) {
            const unsigned r = image.At(x, y, 0), g = image.At(x, y, 1), b = image.At(x, y, 2);
            const bool inside = x >= left + 2 && x <= right - 2 && y >= top + 2 && y <= bottom - 2;
            const bool box = x + 40 >= left && x <= right + 40 && y + 40 >= top && y <= bottom + 40; // The shadow's reach.
            if (!box && (r != 128 || g != 128 || b != 128)) ++look.outside;
            if (!inside) continue;
            if (r < 60 && g < 60 && b < 60) ++look.glass;
            if (r > 200 && g > 200 && b > 200 && x > left + (StatusCue::kPadding + StatusCue::kIconWidth) * u.scale - 2) {
                ++look.ink; look.ink_left = (std::min)(look.ink_left, x); look.ink_right = (std::max)(look.ink_right, x);
            }
            if (r > 150 && r > b + 60) ++look.amber;
        }
    return look;
}
// MEDIA_STATUS_DUMP=<folder>: the pill's top band of each image as a BMP, to look at (not part of the verdict).
void Dump(const Image& image, const char* name) {
    char folder[260]{}; std::size_t length = 0;
    if (getenv_s(&length, folder, sizeof(folder), "MEDIA_STATUS_DUMP") || !length) return;
    char path[400]; std::snprintf(path, sizeof(path), "%s\\%s.bmp", folder, name);
    FILE* f = nullptr; if (fopen_s(&f, path, "wb") || !f) return;
    constexpr unsigned left = 340, width = 600, height = 100; // Around the pill.
    const std::uint32_t data = width * height * 4, size = 54 + data;
    const std::uint8_t header[54]{'B', 'M', static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size >> 16), 0, 0, 0, 0, 0, 54, 0, 0, 0,
        40, 0, 0, 0, static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(width >> 8), 0, 0, static_cast<std::uint8_t>(height), 0, 0, 0, 1, 0, 32, 0};
    std::fwrite(header, 1, sizeof(header), f);
    for (unsigned y = height; y-- > 0;)
        for (unsigned x = left; x < left + width; ++x) { const std::uint8_t bgra[4]{static_cast<std::uint8_t>(image.At(x, y, 2)), static_cast<std::uint8_t>(image.At(x, y, 1)), static_cast<std::uint8_t>(image.At(x, y, 0)), 255}; std::fwrite(bgra, 1, 4, f); }
    std::fclose(f);
}
void Pixels() {
    static Gpu gpu; // Its storage is large; created once.
    const HRESULT prepared = gpu.Prepare();
    if (FAILED(prepared)) { std::printf("SKIP GPU part: no Direct3D 12 device (%08lx)\n", static_cast<unsigned long>(prepared)); return; }
    std::int64_t now = 10 * S;
    allocation_probe::Begin();
    CHECK(gpu.Show("Audio \xc2\xb7 English \xc2\xb7 AAC stereo", StatusIcon::Audio, StatusTone::Info, shown, now));
    const auto latin = Inspect(shown, gpu.cue);
    const auto& stats = gpu.pipeline.Stats();
    const float pill_right = gpu.cue.Uniforms().center_x + gpu.cue.Uniforms().half_width * gpu.cue.Uniforms().scale;
    std::printf("Latin cue: %u ink pixels in columns %u-%u, raster ink %.0f px, pill %.0f px wide; %u glass pixels, %u changed outside\n", latin.ink, latin.ink_left,
                latin.ink_right, stats.ink_right - stats.ink_left, 2 * gpu.cue.Uniforms().half_width, latin.glass, latin.outside);
    // The text is drawn inside the pill, which fits it: padding on the right, nothing outside the pill's box changed.
    CHECK(latin.ink > 150 && latin.glass > 2000 && latin.outside == 0 && latin.ink_right + 4 < pill_right && latin.ink_right + 30 > pill_right - StatusCue::kPadding);
    CHECK(stats.revision == gpu.cue.ShownRevision() && stats.failures == 0);
    // Hidden: not one pixel of it.
    now += 10 * S; gpu.cue.Advance(now);
    CHECK(!gpu.cue.Visible() && gpu.Draw(hidden));
    unsigned changed = 0; for (unsigned i = 0; i < kWidth * kHeight * 4; ++i) changed += hidden.pixels[i] != std::byte{128} && i % 4 != 3 ? 1u : 0u;
    CHECK(changed == 0);
    // Hebrew (right-to-left, with its own font): ink of its own, a narrower pill for a shorter text.
    CHECK(gpu.Show("\xd7\x9b\xd7\xaa\xd7\x95\xd7\x91\xd7\x99\xd7\x95\xd7\xaa \xc2\xb7 \xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa", StatusIcon::Subtitles, StatusTone::Info, hebrew, now));
    const auto rtl = Inspect(hebrew, gpu.cue);
    std::printf("Hebrew cue: %u ink pixels, pill %.0f px wide\n", rtl.ink, 2 * gpu.cue.Uniforms().half_width);
    CHECK(rtl.ink > 80 && rtl.outside == 0 && gpu.cue.Uniforms().half_width < StatusCue::kMaxHalfWidth && rtl.amber == 0);
    // A refusal: the amber rim and warning sign.
    CHECK(gpu.Show("No French audio", StatusIcon::Warning, StatusTone::Warning, warned, now));
    const auto warning = Inspect(warned, gpu.cue);
    std::printf("Warning cue: %u amber pixels (an Info cue has %u)\n", warning.amber, latin.amber);
    CHECK(warning.amber > 60 && latin.amber == 0 && warning.outside == 0);
    // Mid-fade the pill is fainter than held, and still entirely inside its box.
    gpu.cue.Show("Subtitles off", StatusIcon::Subtitles, StatusTone::Info, now + 4 * S);
    CHECK(gpu.pipeline.Settle(gpu.cue, now + 4 * S, 10000));
    gpu.cue.Advance(now + 4 * S + 600000); // 0.06 s into its fade in.
    static Image faint; CHECK(gpu.cue.Visible() && gpu.cue.Uniforms().visibility < 0.7f && gpu.Draw(faint));
    const auto fading = Inspect(faint, gpu.cue);
    CHECK(fading.glass < latin.glass && fading.outside == 0);
    // A panel of three lines (the telemetry readout): three bands of ink rows, one per line, inside a taller glass box,
    // its top edge where the pill's is.
    static Image panel;
    const float pill_top = gpu.cue.Uniforms().center_y - gpu.cue.Uniforms().half_height * gpu.cue.Uniforms().scale;
    now += 10 * S; gpu.cue.Advance(now);
    CHECK(gpu.Show("Quality \xc2\xb7 Edge-directed (auto)\nAudio \xc2\xb7 48 kHz stereo\nFrames \xc2\xb7 120 shown \xc2\xb7 0 dropped", StatusIcon::Device, StatusTone::Info, panel, now));
    const auto block = Inspect(panel, gpu.cue);
    const auto& pu = gpu.cue.Uniforms();
    const float panel_top = pu.center_y - pu.half_height * pu.scale;
    unsigned bands = 0; bool inked = false;
    for (unsigned y = static_cast<unsigned>(panel_top); y < static_cast<unsigned>(pu.center_y + pu.half_height * pu.scale); ++y) {
        bool row = false;
        for (unsigned x = static_cast<unsigned>(pu.center_x - pu.half_width * pu.scale + (StatusCue::kPadding + StatusCue::kIconWidth) * pu.scale);
             x < static_cast<unsigned>(pu.center_x + pu.half_width * pu.scale) && !row; ++x)
            row = panel.At(x, y, 0) > 200 && panel.At(x, y, 1) > 200 && panel.At(x, y, 2) > 200;
        if (row && !inked) ++bands;
        inked = row;
    }
    std::printf("Panel: %u lines of ink, %u ink pixels, %.0f x %.0f px\n", bands, block.ink, 2 * pu.half_width * pu.scale, 2 * pu.half_height * pu.scale);
    CHECK(bands == 3 && block.outside == 0 && gpu.cue.ShownLines() == 3 && std::abs(panel_top - pill_top) < 1.5f);
    CHECK(allocation_probe::End() == 0);
    Dump(shown, "status_audio"); Dump(hebrew, "status_hebrew"); Dump(warned, "status_warning"); Dump(faint, "status_fading"); Dump(panel, "status_panel");
}
#endif
}
int main() {
    std::puts("HUD status cue tests"); // The first output allocates stdout's buffer (seen by the probe in MSVC Debug): before any probe.
    Model();
#if defined(_WIN32)
    Pixels();
#endif
    std::printf("HUD status cues: %d failures\n", failures);
    return failures ? 1 : 0;
}
