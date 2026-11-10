// Offscreen GPU subtitle test: bidi placement, cue boundaries, stacking and allocation-free scrubbing.
// Only the synthetic subtitle render target is read back; no decoded video is involved.
#include "media/render/SdfSubtitlePipeline.hpp"
#include "allocation_probe.hpp"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
constexpr unsigned Width = 960, Height = 540, Bytes = Width * Height * 4;
constexpr std::int64_t Second = 10000000;
using Pixels = std::array<std::byte, Bytes>;
Pixels blank{}, a{}, b{}; // Fixed storage: no runtime heap, no large stack frames.
// Cue indices after sorting by start time (the source is already in order).
enum Cue : unsigned { Bang, HelloShalom, Beh, HebrewDigits, Digits, ShalomHello, MarkupOnly, Abc, High, Low, CueCount };
constexpr auto kCaptions =
    "WEBVTT\n\n"
    "00:00.000 --> 00:01.000\n!\n\n"
    "00:01.000 --> 00:02.000\nHello\n\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d!\n\n"                      // Hello / שלום!
    "00:02.000 --> 00:03.000\n\xd8\xa8 \xd8\xa8\xd8\xa8\xd8\xa8\n\n"                             // ب ببب
    "00:03.000 --> 00:04.000\n\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa 123\n\n"                 // עברית 123
    "00:04.000 --> 00:05.000\n123\n\n"
    "00:05.000 --> 00:06.000\n\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\nHello!\n\n"                      // שלום / Hello!
    "00:06.000 --> 00:07.000\n<i></i>\n\n"
    "00:08.000 --> 00:09.000\nabc\n\n"
    "00:10.000 --> 00:12.000\nHIGH\n\n"
    "00:11.000 --> 00:12.000\nLOW\n";
struct Gpu {
    ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource *target = nullptr, *readback = nullptr; ID3D12DescriptorHeap* rtvs = nullptr; ID3D12InfoQueue* messages = nullptr;
    HANDLE event = nullptr; std::byte* mapped = nullptr; std::uint64_t sequence = 0; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    media::core::LinearMemoryArena arena; media::subtitle::SubtitleTrack track; media::render::SdfSubtitlePipeline subtitles;
    ~Gpu() {
        subtitles.Release(); Release(list); Release(allocator); Release(target);
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
        if (FAILED(hr) || !event) return E_FAIL;
        if (!arena.Initialize(48 * 1024 * 1024) || !track.Prepare(arena, kCaptions)) return E_FAIL;
        hr = subtitles.Prepare(device, queue, arena, track);
        if (SUCCEEDED(hr) && (!arena.Seal() || !subtitles.WaitShaped(10000))) return E_FAIL; // Shaping runs on its own thread.
        return hr;
    }
    // Clears to dark grey, draws the cues active at `time` 40 px above the bottom (once their glyphs are outlined:
    // Settle waits for the shaping thread), and reads the image back.
    bool Draw(std::int64_t time, Pixels& pixels, unsigned& drawn, media::render::SdfSubtitlePipeline* pipeline = nullptr) {
        auto& target_pipeline = pipeline ? *pipeline : subtitles;
        if (!target_pipeline.Settle(time, 10000)) return false;
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) return false;
        constexpr float clear[]{0.1f, 0.1f, 0.1f, 1}; const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        drawn = target_pipeline.Record(list, rtv, Width, Height, time, 40);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = target; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list}; queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence, ++sequence)) || FAILED(fence->SetEventOnCompletion(sequence, event))) return false;
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return false;
        for (unsigned y = 0; y < Height; ++y) std::memcpy(pixels.data() + y * Width * 4, mapped + y * footprint.Footprint.RowPitch, Width * 4);
        return true;
    }
};
unsigned Differences(const Pixels& x, const Pixels& y) { unsigned n = 0; for (unsigned i = 0; i < Bytes; ++i) n += x[i] != y[i]; return n; }
std::uint64_t Hash(const Pixels& p) { std::uint64_t h = 1469598103934665603ull; for (auto v : p) h = (h ^ static_cast<unsigned>(v)) * 1099511628211ull; return h; }
// First and last rows holding text ink (fill is near white; the clear is 26/255).
void InkRows(const Pixels& p, int& top, int& bottom) {
    top = -1; bottom = -1;
    for (unsigned y = 0; y < Height; ++y)
        for (unsigned x = 0; x < Width; ++x)
            if (static_cast<unsigned>(p[(y * Width + x) * 4]) > 200) { if (top < 0) top = static_cast<int>(y); bottom = static_cast<int>(y); break; }
}
using Placement = media::render::SdfSubtitlePipeline::Placement;
struct Line { Placement glyphs[64]; unsigned count = 0; };
// Splits a two-line cue's placements into its first and second line by baseline.
void Lines(const media::render::SdfSubtitlePipeline& s, unsigned cue, Line& first, Line& second) {
    Placement all[64]{}; const unsigned n = s.Placements(cue, all); const float split = s.CueHeight(cue) * 0.5f;
    first.count = second.count = 0;
    for (unsigned i = 0; i < n; ++i) { auto& line = all[i].y < split ? first : second; line.glyphs[line.count++] = all[i]; }
}
bool Decreasing(const Placement* p, unsigned n) { for (unsigned i = 1; i < n; ++i) if (!(p[i].x < p[i - 1].x)) return false; return n > 1; }
bool Increasing(const Placement* p, unsigned n) { for (unsigned i = 1; i < n; ++i) if (!(p[i].x > p[i - 1].x)) return false; return n > 1; }
void Bidi(const media::render::SdfSubtitlePipeline& s) {
    const auto& stats = s.Stats();
    std::printf("Shaping: glyphs=%u instances=%u runs=%u RTL runs=%u RTL lines=%u fonts=%u missing=%u\n", stats.glyphs, stats.instances,
                stats.runs, stats.rtl_runs, stats.rtl_lines, stats.font_faces, stats.missing);
    CHECK(stats.missing == 0);
    // Right-to-left lines: שלום!, ب ببب, עברית 123 and שלום. "Hello!" after a Hebrew line is left-to-right again.
    CHECK(stats.rtl_lines == 4);
    Placement one[4]{}; CHECK(s.Placements(Bang, one) == 1); const unsigned bang = one[0].glyph;
    Placement digits[4]{}; CHECK(s.Placements(Digits, digits) == 3); CHECK(Increasing(digits, 3));
    // "Hello" / "שלום!": the Hebrew line is its own RTL paragraph, so its trailing "!" is on the far left.
    Line first, second; Lines(s, HelloShalom, first, second);
    CHECK(first.count == 5 && Increasing(first.glyphs, 5) && first.glyphs[2].glyph == first.glyphs[3].glyph); // H e l l o.
    CHECK(second.count == 5);
    unsigned found = 0; float bang_x = 0, hebrew_min = 1e9f;
    for (unsigned i = 0; i < second.count; ++i) {
        if (second.glyphs[i].glyph == bang) { ++found; bang_x = second.glyphs[i].x; }
        else hebrew_min = (std::min)(hebrew_min, second.glyphs[i].x);
    }
    CHECK(found == 1 && bang_x < hebrew_min);
    // "שלום" / "Hello!": the reverse; RTL glyphs advance leftwards in logical order, the LTR line's "!" is last.
    Lines(s, ShalomHello, first, second);
    CHECK(first.count == 4 && Decreasing(first.glyphs, 4));
    CHECK(second.count == 6 && Increasing(second.glyphs, 6) && second.glyphs[5].glyph == bang);
    // Arabic joining: isolated beh, then initial, medial and final beh are four distinct contextual glyphs, right to left.
    Placement beh[8]{}; CHECK(s.Placements(Beh, beh) == 4); CHECK(Decreasing(beh, 4));
    bool distinct = true;
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = i + 1; j < 4; ++j) distinct = distinct && beh[i].glyph != beh[j].glyph;
    CHECK(distinct);
    // "עברית 123": European digits stay left-to-right inside the RTL line and sit to the left of the Hebrew word.
    Placement mixed[16]{}; const unsigned n = s.Placements(HebrewDigits, mixed); CHECK(n == 8);
    Placement numbers[3]{}, hebrew[5]{}; unsigned numbers_n = 0, hebrew_n = 0;
    for (unsigned i = 0; i < n; ++i) {
        const bool digit = mixed[i].glyph == digits[0].glyph || mixed[i].glyph == digits[1].glyph || mixed[i].glyph == digits[2].glyph;
        if (digit && numbers_n < 3) numbers[numbers_n++] = mixed[i]; else if (!digit && hebrew_n < 5) hebrew[hebrew_n++] = mixed[i];
    }
    CHECK(numbers_n == 3 && hebrew_n == 5);
    CHECK(numbers[0].glyph == digits[0].glyph && numbers[2].glyph == digits[2].glyph && Increasing(numbers, 3));
    CHECK(Decreasing(hebrew, 5) && numbers[2].x < hebrew[4].x);
    Placement none[4]{}; CHECK(s.Placements(MarkupOnly, none) == 0 && s.CueHeight(MarkupOnly) == 0);
    Placement abc[4]{}; CHECK(s.Placements(Abc, abc) == 3 && Increasing(abc, 3));
}
}
// A track needing more distinct glyphs than the atlas holds: 80 cues of 0.1 s, each "字幕 " (in every cue) followed by
// 55 ideographs used nowhere else (4402 distinct against 2048 cells). Preparation does no font work; every cue must
// then draw every one of its glyphs as itself (the atlas pages glyphs in and out; nothing is replaced by a dot), and a
// cue drawn again after its glyphs were evicted must come back bit-identical.
constexpr unsigned kOverflowCues = 80, kUnique = 55;
char overflow_text[kOverflowCues * (64 + 58 * 3)]{};
media::core::LinearMemoryArena overflow_arena; media::subtitle::SubtitleTrack overflow_track; media::render::SdfSubtitlePipeline overflow;
Pixels first_cue{};
unsigned Ink(const Pixels& p) { unsigned n = 0; for (unsigned i = 0; i < Bytes; i += 4) n += static_cast<unsigned>(p[i]) > 200; return n; }
void OverflowPrepare(Gpu& gpu) {
    std::size_t n = 0; const auto put = [&](const char* text) { while (*text) overflow_text[n++] = *text++; };
    put("WEBVTT\n\n");
    for (unsigned c = 0; c < kOverflowCues; ++c) {
        char timing[48]{}; const int w = std::snprintf(timing, sizeof(timing), "00:%02u.%03u --> 00:%02u.%03u\n", c / 10, c % 10 * 100, (c + 1) / 10, (c + 1) % 10 * 100);
        for (int k = 0; k < w; ++k) overflow_text[n++] = timing[k];
        put("\xe5\xad\x97\xe5\xb9\x95 "); // 字幕
        for (unsigned k = 0; k < kUnique; ++k) {
            const unsigned scalar = 0x6000 + c * kUnique + k; // U+6000..U+7133: distinct, and apart from 字 and 幕.
            overflow_text[n++] = static_cast<char>(0xe0 | (scalar >> 12)); overflow_text[n++] = static_cast<char>(0x80 | ((scalar >> 6) & 63));
            overflow_text[n++] = static_cast<char>(0x80 | (scalar & 63));
        }
        put("\n\n");
    }
    CHECK(overflow_arena.Initialize(48 * 1024 * 1024) && overflow_track.Prepare(overflow_arena, {overflow_text, n}));
    LARGE_INTEGER f{}, t0{}, t1{}, t2{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    const HRESULT hr = overflow.Prepare(gpu.device, gpu.queue, overflow_arena, overflow_track); QueryPerformanceCounter(&t1);
    CHECK(SUCCEEDED(hr) && overflow_arena.Seal() && overflow.WaitShaped(20000)); QueryPerformanceCounter(&t2);
    const auto ms = [&](LARGE_INTEGER a0, LARGE_INTEGER a1) { return static_cast<double>(a1.QuadPart - a0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart); };
    const auto st = overflow.Stats();
    std::printf("Overflow: Prepare %.1f ms, all %u cues shaped after %.0f ms (background); cells=%u distinct=%u dropped=%u fonts=%u\n",
                ms(t0, t1), st.shaped, ms(t0, t2), st.cells, st.unique, st.dropped, st.font_faces);
    CHECK(ms(t0, t1) < 20); // No font work before the first frame.
    CHECK(st.cells == media::render::SdfSubtitlePipeline::kAtlasCells && st.unique >= 2 + kOverflowCues * kUnique && st.dropped == 0);
    // The shared phrase is the same two glyphs in every cue; every other glyph is distinct.
    Placement p[64]{}, phrase[2]{}; unsigned same_phrase = 0;
    for (unsigned c = 0; c < kOverflowCues; ++c) {
        const unsigned placed = overflow.Placements(c, p); CHECK(placed == 2 + kUnique);
        if (!c) { phrase[0] = p[0]; phrase[1] = p[1]; }
        same_phrase += p[0].glyph == phrase[0].glyph && p[1].glyph == phrase[1].glyph && p[0].glyph != p[1].glyph;
    }
    CHECK(same_phrase == kOverflowCues);
}
// Every cue in order, then the first again. Ink is counted per cue: an ideograph at this size leaves over a hundred
// bright pixels, a dot a handful.
void Overflow(Gpu& gpu) {
    unsigned drawn = 0, least = ~0u, most = 0, whole = 0;
    for (unsigned c = 0; c < kOverflowCues; ++c) {
        if (!gpu.Draw(c * Second / 10 + Second / 20, a, drawn, &overflow)) { CHECK(false); break; }
        const unsigned ink = Ink(a); least = (std::min)(least, ink); most = (std::max)(most, ink); whole += drawn == 1;
        if (!c) first_cue = a;
    }
    CHECK(gpu.Draw(Second / 20, b, drawn, &overflow) && drawn == 1);
    const unsigned repeat = Differences(first_cue, b);
    const auto st = overflow.Stats();
    std::printf("Overflow draws: %u of %u cues whole; ink %u..%u px per cue; %u cells generated, %u evictions; first cue again: %u px differ\n",
                whole, kOverflowCues, least, most, st.generated, st.evictions, repeat);
    CHECK(whole == kOverflowCues && least >= (2 + kUnique) * 60 && least * 2 >= most && repeat == 0);
    CHECK(st.evictions > 0 && st.generated >= 2 + kOverflowCues * kUnique && st.dropped == 0);
    CHECK(overflow.Status() == media::render::SubtitleStatus::Ready);
}
int main() {
    Gpu gpu; const HRESULT hr = gpu.Prepare();
    if (FAILED(hr)) { std::printf("Subtitle GPU preparation failed %08lx\n", static_cast<unsigned long>(hr)); return 1; }
    CHECK(gpu.track.Cues().size() == CueCount);
    Bidi(gpu.subtitles);
    CHECK(gpu.subtitles.Status() == media::render::SubtitleStatus::Ready);
    OverflowPrepare(gpu);
    allocation_probe::Begin(); // Paging, generation and scrubbing: nothing may allocate.
    Overflow(gpu);
    unsigned drawn = 0;
    // Boundaries are half-open [start, end): one 100 ns tick decides.
    CHECK(gpu.Draw(7 * Second + Second / 2, blank, drawn) && drawn == 0);
    CHECK(gpu.Draw(6 * Second + Second / 2, a, drawn) && drawn == 1 && Differences(a, blank) == 0); // Markup only: selected, no ink.
    CHECK(gpu.Draw(8 * Second - 1, a, drawn) && drawn == 0 && Differences(a, blank) == 0);
    CHECK(gpu.Draw(8 * Second, a, drawn) && drawn == 1 && Differences(a, blank) > 200);
    CHECK(gpu.Draw(9 * Second - 1, b, drawn) && drawn == 1 && Differences(a, b) == 0);
    CHECK(gpu.Draw(9 * Second, b, drawn) && drawn == 0 && Differences(b, blank) == 0);
    // Back-to-back cues switch on the shared tick: "!" ends exactly where "Hello / שלום!" starts.
    CHECK(gpu.Draw(Second - 1, a, drawn) && drawn == 1); CHECK(gpu.Draw(Second, b, drawn) && drawn == 1 && Differences(a, b) > 200);
    // Overlapping cues stack upwards: the later cue keeps the bottom line, the earlier one moves above it.
    int top_single = 0, bottom_single = 0, top_stacked = 0, bottom_stacked = 0;
    CHECK(gpu.Draw(10 * Second + Second / 2, a, drawn) && drawn == 1); InkRows(a, top_single, bottom_single);
    CHECK(gpu.Draw(11 * Second + Second / 2, b, drawn) && drawn == 2); InkRows(b, top_stacked, bottom_stacked);
    std::printf("Stacking: single rows %d-%d, stacked rows %d-%d\n", top_single, bottom_single, top_stacked, bottom_stacked);
    CHECK(top_single > 0 && std::abs(bottom_stacked - bottom_single) <= 1 && top_stacked < top_single - 20);
    CHECK(bottom_single < static_cast<int>(Height) - 40); // Clear of the bottom inset.
    // Scrubbing: jump across the timeline in both directions. Every cue set must render bit-identically whatever
    // was drawn before it (no retained per-frame state), and nothing may allocate.
    struct Seen { std::uint64_t stamp, hash; };
    Seen seen[64]{}; unsigned seen_n = 0, mismatches = 0, draws = 0, empty_ink = 0;
    std::uint32_t random = 12345;
    const std::uint64_t blank_hash = Hash(blank);
    for (unsigned i = 0; i < 360; ++i) {
        random = random * 1664525u + 1013904223u;
        // Mix uniform positions with exact boundaries and their preceding tick.
        std::int64_t t = static_cast<std::int64_t>(random) * 13 * Second >> 32;
        if (i % 3 == 1) t = (random >> 8) % 13 * Second; else if (i % 3 == 2) t = (random >> 8) % 13 * Second - 1;
        unsigned expected[media::subtitle::SubtitleTrack::kMaxActive]{};
        const unsigned count = gpu.track.Select(t, expected);
        std::uint64_t stamp = 0; for (unsigned k = 0; k < count; ++k) stamp = stamp * 4097 + expected[k] + 1;
        if (!gpu.Draw(t, a, drawn)) { CHECK(false); break; }
        ++draws; if (drawn != count) ++mismatches;
        const auto hash = Hash(a);
        const bool visible = count && !(count == 1 && expected[0] == MarkupOnly);
        if (visible == (hash == blank_hash)) ++empty_ink;
        unsigned k = 0; while (k < seen_n && seen[k].stamp != stamp) ++k;
        if (k == seen_n && seen_n < 64) seen[seen_n++] = {stamp, hash};
        else if (k < seen_n && seen[k].hash != hash) ++mismatches;
    }
    const auto allocations = allocation_probe::End();
    std::printf("Scrub: %u draws over %u distinct cue sets, %u mismatches, %u ink errors, %llu allocations\n", draws, seen_n, mismatches,
                empty_ink, static_cast<unsigned long long>(allocations));
    CHECK(draws == 360 && seen_n >= 10 && mismatches == 0 && empty_ink == 0 && allocations == 0);
    if (gpu.messages) {
        // Any D3D12 validation error is printed (setup-time buffer; the probe has ended).
        for (UINT64 i = 0, n = gpu.messages->GetNumStoredMessages(); i < n && i < 5; ++i) {
            static std::byte storage[4096]; SIZE_T length = sizeof(storage); auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage);
            if (SUCCEEDED(gpu.messages->GetMessage(i, message, &length))) std::printf("D3D12: %s\n", message->pDescription);
        }
        CHECK(gpu.messages->GetNumStoredMessages() == 0);
    }
    overflow.Release();
    std::printf("SDF subtitles GPU: %d failures\n", failures);
    return failures ? 1 : 0;
}
