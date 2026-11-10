// The A–B loop (docs/seeking.md, "A–B loop"): the portable model (the L key's three steps, either end set by a command, the
// sentence loop's padding, refusals, when a turn is due, the dock's packing), the commands in nine languages, and on Windows
// the dock's bracket as drawn by HudComposite.hlsl (offscreen: a mint bracket at A, one at B, the stretch between lit,
// nothing else changed). No allocation after setup.
#include "media/timeline/LoopRegion.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "allocation_probe.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string_view>
#if defined(_WIN32)
#include "media/render/HudPipeline.hpp"
#include "media/ui/KineticHud.hpp"
#include <array>
#endif
using media::timeline::LoopRegion;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr std::int64_t S = 10000000, D = 30 * S; // A 30 s media.
void Model() {
    using Step = LoopRegion::Step; using State = LoopRegion::State;
    LoopRegion loop;
    // The L key: A, B, off.
    CHECK(loop.Current() == State::Off && !loop.Looping() && loop.Packed(D) == 0);
    CHECK(loop.Toggle(12 * S, D) == Step::MarkedStart && loop.Current() == State::Armed && loop.A() == 12 * S);
    CHECK(!loop.Due(40 * S)); // Armed is not looping.
    CHECK((loop.Packed(D) >> 16) == 0 && (loop.Packed(D) & 0xffff) == LoopRegion::Fraction(12 * S, D));
    CHECK(loop.Toggle(15 * S, D) == Step::Looping && loop.Looping() && loop.A() == 12 * S && loop.B() == 15 * S && !loop.FromCue());
    CHECK(!loop.Due(15 * S - 1) && loop.Due(15 * S) && loop.Due(29 * S)); // Reaching B, or past it after a seek, turns back.
    CHECK(loop.Inside(12 * S) && loop.Inside(15 * S - 1) && !loop.Inside(15 * S) && !loop.Inside(11 * S));
    loop.NoteWrap(); loop.NoteWrap(); CHECK(loop.Wraps() == 2);
    CHECK(loop.Packed(D) == (LoopRegion::Fraction(12 * S, D) | static_cast<std::uint32_t>(LoopRegion::Fraction(15 * S, D)) << 16));
    CHECK(loop.Toggle(16 * S, D) == Step::Cleared && loop.Current() == State::Off && loop.Packed(D) == 0 && !loop.Due(40 * S));
    // Positions along the track: the start is 1, the end 65535; 0 is no mark.
    CHECK(LoopRegion::Fraction(0, D) == 1 && LoopRegion::Fraction(D, D) == 65535 && LoopRegion::Fraction(-5, D) == 1 && LoopRegion::Fraction(D + 5, D) == 65535);
    CHECK(LoopRegion::Fraction(D / 2, D) == 32768);
    // B before A: the other way round. Too short: refused, nothing changed.
    CHECK(loop.SetStart(20 * S, D) == Step::MarkedStart);
    CHECK(loop.SetEnd(20 * S + LoopRegion::kMinimum - 1, D) == Step::TooShort && loop.Current() == State::Armed && loop.A() == 20 * S);
    CHECK(loop.SetEnd(18 * S, D) == Step::Looping && loop.A() == 18 * S && loop.B() == 20 * S);
    // "set loop end" with nothing marked; positions and media it cannot use.
    loop.Clear();
    CHECK(loop.SetEnd(5 * S, D) == Step::NoStart && loop.Current() == State::Off);
    CHECK(loop.SetStart(-1, D) == Step::Unavailable && loop.SetStart(D + 1, D) == Step::Unavailable && loop.SetStart(S, 0) == Step::Unavailable);
    CHECK(loop.Current() == State::Off);
    // A loop set while looping: "set loop start" arms a new one from there (the old B forgotten).
    CHECK(loop.SetStart(2 * S, D) == Step::MarkedStart && loop.SetEnd(4 * S, D) == Step::Looping && loop.SetStart(6 * S, D) == Step::MarkedStart);
    CHECK(loop.Current() == State::Armed && loop.A() == 6 * S && loop.Wraps() == 0);
    // The sentence: the cue padded by 150 ms on both sides, inside the media.
    CHECK(loop.SetCue(14 * S, 16 * S + 5000000, D) == Step::Looping && loop.FromCue());
    CHECK(loop.A() == 14 * S - LoopRegion::kCuePad && loop.B() == 16 * S + 5000000 + LoopRegion::kCuePad);
    CHECK(loop.SetCue(1000000, 2 * S, D) == Step::Looping && loop.A() == 0);              // A cue at the very start.
    CHECK(loop.SetCue(D - S, D, D) == Step::Looping && loop.B() == D);                     // And at the very end.
    CHECK(loop.SetCue(5 * S, 5 * S, D) == Step::Unavailable && loop.SetCue(D, D + S, D) == Step::Unavailable);
    CHECK(loop.Looping() && loop.A() == D - S - LoopRegion::kCuePad); // Refusals left the last loop as it was.
    CHECK(loop.Toggle(0, D) == Step::Cleared);
}
// The commands: each language's way of saying them reaches the loop and telemetry intents, refused when the media cannot.
void Commands() {
    using media::ai::IntentKind; using media::ai::IntentStatus; using Dispatcher = media::ai::LocalIntentDispatcher;
    media::ai::MediaBounds bounds; bounds.duration_100ns = D; bounds.position_100ns = 12 * S; bounds.seekable = true; bounds.subtitles = true;
    struct Case { const char* text; IntentKind kind; };
    static constexpr Case cases[]{
        {"set loop start", IntentKind::LoopStart}, {"Loop start.", IntentKind::LoopStart}, {"\xd7\xaa\xd7\x97\xd7\x99\xd7\x9c\xd7\xaa \xd7\x9c\xd7\x95\xd7\xa4", IntentKind::LoopStart},
        {"set loop end", IntentKind::LoopEnd}, {"\xd7\xa1\xd7\x95\xd7\xa3 \xd7\x9c\xd7\x95\xd7\xa4", IntentKind::LoopEnd}, {"punto b", IntentKind::LoopEnd},
        {"stop the loop", IntentKind::LoopClear}, {"\xd7\x91\xd7\x98\xd7\x9c \xd7\x9c\xd7\x95\xd7\xa4", IntentKind::LoopClear}, {"\xe5\x8f\x96\xe6\xb6\x88\xe5\xbe\xaa\xe7\x8e\xaf", IntentKind::LoopClear},
        {"loop this sentence", IntentKind::LoopCue}, {"repeat this line", IntentKind::LoopCue}, {"can you loop this line please", IntentKind::LoopCue},
        {"\xd7\x9c\xd7\x95\xd7\xa4 \xd7\xa2\xd7\x9c \xd7\x94\xd7\x9e\xd7\xa9\xd7\xa4\xd7\x98", IntentKind::LoopCue},
        {"\xd7\x97\xd7\x96\xd7\x95\xd7\xa8 \xd7\xa2\xd7\x9c \xd7\x94\xd7\x9e\xd7\xa9\xd7\xa4\xd7\x98 \xd7\x91\xd7\x9c\xd7\x95\xd7\xa4", IntentKind::LoopCue},
        {"show stats", IntentKind::ShowStats}, {"Status report", IntentKind::ShowStats}, {"\xd7\x94\xd7\xa6\xd7\x92 \xd7\xa0\xd7\xaa\xd7\x95\xd7\xa0\xd7\x99\xd7\x9d", IntentKind::ShowStats},
        {"hide stats", IntentKind::HideStats}, {"why did quality change?", IntentKind::ExplainQuality},
        {"\xd7\x9c\xd7\x9e\xd7\x94 \xd7\x94\xd7\x90\xd7\x99\xd7\x9b\xd7\x95\xd7\xaa \xd7\x99\xd7\xa8\xd7\x93\xd7\x94?", IntentKind::ExplainQuality},
        {"why did the quality drop", IntentKind::ExplainQuality},
    };
    for (const auto& c : cases) {
        const auto intent = Dispatcher::Parse(c.text, bounds);
        if (intent.kind != c.kind || intent.status != IntentStatus::Ok) std::printf("  \"%s\": %s %s\n", c.text, media::ai::Name(intent.kind), media::ai::Name(intent.status));
        CHECK(intent.kind == c.kind && intent.status == IntentStatus::Ok);
    }
    // The line on screen again stays the replay it was; "again" stays a repeat.
    CHECK(Dispatcher::Parse("repeat the line", bounds).kind == IntentKind::ReplayCue);
    CHECK(Dispatcher::Parse("\xd7\x97\xd7\x96\xd7\x95\xd7\xa8 \xd7\xa2\xd7\x9c \xd7\x94\xd7\x9e\xd7\xa9\xd7\xa4\xd7\x98", bounds).kind == IntentKind::ReplayCue);
    CHECK(Dispatcher::Parse("again", bounds).kind == IntentKind::Repeat);
    // Refusals: a loop turns back by seeking; a sentence loop needs subtitles.
    auto fixed = bounds; fixed.seekable = false;
    CHECK(Dispatcher::Parse("set loop start", fixed).status == IntentStatus::Unavailable && Dispatcher::Parse("loop this sentence", fixed).status == IntentStatus::Unavailable);
    auto silent = bounds; silent.subtitles = false;
    CHECK(Dispatcher::Parse("loop this sentence", silent).status == IntentStatus::Unavailable && Dispatcher::Parse("set loop end", silent).status == IntentStatus::Ok);
    CHECK(Dispatcher::Parse("show stats", fixed).status == IntentStatus::Ok); // The readout needs nothing.
    // A surplus word is not taken for part of the command ("now", a softener, would be set aside).
    const auto surplus = Dispatcher::Parse("set loop start the movie", bounds);
    if (surplus.status != IntentStatus::Malformed) std::printf("  \"set loop start the movie\": %s %s\n", media::ai::Name(surplus.kind), media::ai::Name(surplus.status));
    CHECK(surplus.status == IntentStatus::Malformed);
}
#if defined(_WIN32)
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
constexpr unsigned Width = 1280, Height = 720, Bytes = Width * Height * 4;
using Image = std::array<std::byte, Bytes>;
Image plain, armed, looped; // Fixed test storage.
struct Gpu {
    ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource *target = nullptr, *readback = nullptr; ID3D12DescriptorHeap* rtvs = nullptr;
    media::render::HudPipeline hud; HANDLE event = nullptr; std::byte* mapped = nullptr;
    std::uint64_t sequence = 0; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ~Gpu() {
        hud.Release(); Release(list); Release(allocator); Release(target);
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
        if (SUCCEEDED(hr)) hr = hud.Create(device);
        D3D12_DESCRIPTOR_HEAP_DESC rtv{}; rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv.NumDescriptors = 1;
        if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvs));
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = Width; texture.Height = Height;
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
        return SUCCEEDED(hr) && event ? S_OK : E_FAIL;
    }
    bool Draw(const media::ui::HudUniforms& u, Image& pixels) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) return false;
        const float grey[]{0.15f, 0.15f, 0.15f, 1}; const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        list->ClearRenderTargetView(rtv, grey, 0, nullptr);
        hud.Record(list, rtv, Width, Height, u);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = target; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET; list->ResourceBarrier(1, &b);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence, ++sequence)) || FAILED(fence->SetEventOnCompletion(sequence, event))) return false;
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return false;
        for (unsigned y = 0; y < Height; ++y) std::memcpy(pixels.data() + std::size_t{y} * Width * 4, mapped + std::size_t{y} * footprint.Footprint.RowPitch, std::size_t{Width} * 4);
        return true;
    }
};
unsigned At(const Image& p, unsigned x, unsigned y, unsigned c) { return static_cast<unsigned>(p[(std::size_t{y} * Width + x) * 4 + c]); }
// Mint (the bracket's colour): green well above red, above blue. The dock's own colours are white, grey and a pale blue.
bool Mint(const Image& p, unsigned x, unsigned y) { const unsigned r = At(p, x, y, 0), g = At(p, x, y, 1), b = At(p, x, y, 2); return g > 120 && g > r + 60 && g > b + 25; }
unsigned MintNear(const Image& p, float x, float y, float reach) {
    unsigned n = 0;
    for (int dy = -static_cast<int>(reach); dy <= static_cast<int>(reach); ++dy)
        for (int dx = -static_cast<int>(reach); dx <= static_cast<int>(reach); ++dx) n += Mint(p, static_cast<unsigned>(x + dx), static_cast<unsigned>(y + dy)) ? 1u : 0u;
    return n;
}
void Bracket() {
    static Gpu gpu;
    const HRESULT prepared = gpu.Prepare();
    if (FAILED(prepared)) { std::printf("SKIP GPU part: no Direct3D 12 device (%08lx)\n", static_cast<unsigned long>(prepared)); return; }
    media::ui::KineticHud model; model.Configure(Width, Height, D, true);
    auto u = model.Uniforms(); u.visibility = 1; u.progress = 0.5f; u.elapsed_seconds = 15; u.duration_seconds = 30;
    LoopRegion loop;
    allocation_probe::Begin();
    CHECK(gpu.Draw(u, plain));
    (void)loop.SetStart(6 * S, D); u.loop = loop.Packed(D); CHECK(gpu.Draw(u, armed));
    (void)loop.SetEnd(21 * S, D); u.loop = loop.Packed(D); CHECK(gpu.Draw(u, looped));
    const auto pixels = allocation_probe::End();
    // Where A and B are along the track (HudComposite.hlsl: halfTrack = dockHalfWidth - 28).
    const float half = (u.dock_half_width - 28) * u.scale, y = u.center_y + u.timeline_y * u.scale;
    const float ax = u.center_x - half + 2 * half * 6.0f / 30.0f, bx = u.center_x - half + 2 * half * 21.0f / 30.0f, mid = (ax + bx) / 2;
    const unsigned plain_mint = MintNear(plain, ax, y, 8) + MintNear(plain, bx, y, 8) + MintNear(plain, mid, y, 8);
    const unsigned armed_a = MintNear(armed, ax, y, 8), armed_b = MintNear(armed, bx, y, 8);
    const unsigned looped_a = MintNear(looped, ax, y, 8), looped_b = MintNear(looped, bx, y, 8);
    // The stretch between them: lit, a band along the track's row.
    unsigned band = 0, changed_outside = 0;
    // Translucent, a little taller than the track (whose elapsed part is nearly as green): along its edge row, over the
    // glass, greener than the same row with A alone (no stretch yet), green rising more than red.
    for (unsigned x = static_cast<unsigned>(ax + 10); x < static_cast<unsigned>(bx - 10); ++x) {
        const auto yy = static_cast<unsigned>(y - 2 * u.scale);
        const int green = static_cast<int>(At(looped, x, yy, 1)) - static_cast<int>(At(armed, x, yy, 1));
        const int red = static_cast<int>(At(looped, x, yy, 0)) - static_cast<int>(At(armed, x, yy, 0));
        band += green >= 10 && green > red + 4 ? 1u : 0u;
    }
    for (unsigned yy = 0; yy < Height; ++yy)
        for (unsigned x = 0; x < Width; ++x) {
            if (std::abs(static_cast<float>(yy) - y) <= 14 * u.scale) continue; // The track's row and the brackets' reach.
            for (unsigned c = 0; c < 3; ++c) if (At(plain, x, yy, c) != At(looped, x, yy, c)) { ++changed_outside; break; }
        }
    std::printf("Bracket: mint pixels without a loop %u; A alone %u at A, %u at B; looped %u at A, %u at B, %u of the stretch's row; %u changed elsewhere; %llu allocations\n",
                plain_mint, armed_a, armed_b, looped_a, looped_b, band, changed_outside, static_cast<unsigned long long>(pixels));
    // MEDIA_LOOP_DUMP=<folder>: the dock with the loop as a BMP, to look at (not part of the verdict).
    char folder[260]{}; std::size_t length = 0;
    if (!getenv_s(&length, folder, sizeof(folder), "MEDIA_LOOP_DUMP") && length) {
        char path[400]; std::snprintf(path, sizeof(path), "%s\\loop_dock.bmp", folder);
        FILE* f = nullptr;
        if (!fopen_s(&f, path, "wb") && f) {
            constexpr unsigned left = 300, top = 560, width = 680, height = 160;
            const std::uint32_t size = 54 + width * height * 4;
            const std::uint8_t header[54]{'B', 'M', static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size >> 16), 0, 0, 0, 0, 0, 54, 0, 0, 0,
                40, 0, 0, 0, static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(width >> 8), 0, 0, static_cast<std::uint8_t>(height), 0, 0, 0, 1, 0, 32, 0};
            std::fwrite(header, 1, sizeof(header), f);
            for (unsigned yy = top + height; yy-- > top;)
                for (unsigned x = left; x < left + width; ++x) { const std::uint8_t bgra[4]{static_cast<std::uint8_t>(At(looped, x, yy, 2)), static_cast<std::uint8_t>(At(looped, x, yy, 1)), static_cast<std::uint8_t>(At(looped, x, yy, 0)), 255}; std::fwrite(bgra, 1, 4, f); }
            std::fclose(f);
        }
    }
    CHECK(plain_mint == 0 && armed_a > 10 && armed_b == 0 && looped_a > 10 && looped_b > 10);
    CHECK(band > static_cast<unsigned>((bx - ax - 20) * 0.9f) && changed_outside == 0 && pixels == 0);
}
#endif
}
int main() {
    std::puts("A-B loop tests"); // The first output allocates stdout's buffer: before any probe.
    allocation_probe::Begin();
    Model();
    Commands();
    const auto allocations = allocation_probe::End();
    std::printf("Loop model and commands: %llu allocations\n", static_cast<unsigned long long>(allocations));
    CHECK(allocations == 0);
#if defined(_WIN32)
    Bracket();
#endif
    std::printf("A-B loop: %d failures\n", failures);
    return failures ? 1 : 0;
}
