// Offscreen procedural UI test only. No decoded video is ever read back.
#include "media/render/HudPipeline.hpp"
#include "media/render/SdfSubtitlePipeline.hpp"
#include "allocation_probe.hpp"
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstring>
#include <array>
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
constexpr unsigned Width = 1280, Height = 720, Bytes = Width * Height * 4;
struct Test {
    ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource *target = nullptr, *readback = nullptr; ID3D12DescriptorHeap* rtvs = nullptr;
    ID3D12InfoQueue* messages = nullptr;
    media::core::LinearMemoryArena arena; media::subtitle::SubtitleTrack track;
    media::render::SdfSubtitlePipeline subtitles; unsigned width = Width, height = Height;
    media::render::HudPipeline hud; HANDLE event = nullptr; std::byte* mapped = nullptr;
    std::uint64_t sequence = 0; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ~Test() {
        subtitles.Release(); hud.Release(); Release(list); Release(allocator); Release(target);
        if (readback && mapped) readback->Unmap(0, nullptr);
        Release(readback); Release(rtvs); Release(fence); Release(queue); Release(messages); Release(device);
        if (event) CloseHandle(event);
    }
    HRESULT Prepare(unsigned w = Width, unsigned h = Height) {
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
        if (SUCCEEDED(hr)) hr = hud.Create(device);
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
        if (!arena.Initialize(32*1024*1024)) return E_OUTOFMEMORY;
        constexpr auto captions = "WEBVTT\n\n00:00.000 --> 00:02.000\nThe quiet between the frames.\nA little more room for the story.\n\n00:02.000 --> 00:04.000\nالعربية — مرحبًا بالعالم\nעברית — שלום עולם\n\n00:04.000 --> 00:06.000\nहिन्दी · ภาษาไทย\n日本語 · 中文 · 한국어\n";
        if (!track.Prepare(arena,captions)) return E_FAIL;
        hr = subtitles.Prepare(device,queue,arena,track);
        if (SUCCEEDED(hr) && (!arena.Seal() || !subtitles.WaitShaped(10000))) return E_FAIL; // Shaping runs on its own thread.
        return hr;
    }
    bool Draw(const media::ui::HudUniforms& u, std::array<std::byte, Bytes>& pixels, float background = 0.15f, std::int64_t subtitle_time = -1,
              const media::ui::TimelineMarks& marks = {}) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) return false;
        const float clear[]{background, background, background, 1}; const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        list->ClearRenderTargetView(rtv, clear, 0, nullptr); if (subtitle_time >= 0 && subtitles.Settle(subtitle_time,5000)) (void)subtitles.Record(list,rtv,width,height,subtitle_time,40+u.visibility*(static_cast<float>(height)-u.center_y+u.dock_half_height*u.scale-20));
        hud.Record(list, rtv, width, height, u, marks);
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
        pixels.fill(std::byte{0});
        for (unsigned y = 0; y < height; ++y) std::memcpy(pixels.data() + y * width * 4, mapped + y * footprint.Footprint.RowPitch, width * 4);
        return true;
    }
};
std::array<std::byte, Bytes> a{}, b{}, c{}, clear{}; // Fixed test storage; avoid large stack or runtime heap.
unsigned Differences(const std::array<std::byte, Bytes>& x, const std::array<std::byte, Bytes>& y) {
    unsigned count = 0; for (unsigned i = 0; i < Bytes; ++i) if (x[i] != y[i]) ++count; return count;
}

unsigned Pixel(const std::array<std::byte,Bytes>& p, unsigned x, unsigned y, unsigned width = Width) { return static_cast<unsigned>(p[(y*width+x)*4]); }
void Save(const char* directory, const char* name, const std::array<std::byte,Bytes>& pixels, unsigned width = Width, unsigned height = Height) {
    char path[1024]{}; std::snprintf(path,sizeof(path),"%s/%s.bmp",directory,name);
    FILE* file = nullptr; if (fopen_s(&file,path,"wb") || !file) { CHECK(false); return; }
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header)+sizeof(BITMAPINFOHEADER); header.bfSize = header.bfOffBits+width*height*4;
    BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = static_cast<LONG>(width); info.biHeight = -static_cast<LONG>(height); info.biPlanes = 1; info.biBitCount = 32;
    std::fwrite(&header,sizeof(header),1,file); std::fwrite(&info,sizeof(info),1,file);
    std::array<std::byte,Width*4> row{};
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) { const auto source=(y*width+x)*4; row[x*4]=pixels[source+2]; row[x*4+1]=pixels[source+1]; row[x*4+2]=pixels[source]; row[x*4+3]=std::byte{255}; }
        std::fwrite(row.data(),width*4,1,file);
    }
    std::fclose(file);
}
}
int main(int argc, char** argv) {
    Test t; const auto hr = t.Prepare(); if (FAILED(hr)) { std::printf("HUD/subtitle GPU preparation failed %08lx\n",static_cast<unsigned long>(hr)); return 1; }
    const auto& stats = t.subtitles.Stats();
    CHECK(stats.rtl_runs >= 2 && stats.runs > 6 && stats.unique > 50 && stats.font_faces >= 2);
    std::printf("Shaping: glyphs=%u instances=%u runs=%u RTL=%u fonts=%u missing=%u\n",stats.unique,stats.instances,stats.runs,stats.rtl_runs,stats.font_faces,stats.missing);
    media::ui::KineticHud model; model.Configure(Width,Height,37250000000ll,true);
    auto u = model.Uniforms(); u.progress=0.38f; u.elapsed_seconds=1415; u.freeze=1; u.energy=0;
    allocation_probe::Begin();
    u.visibility=0; CHECK(t.Draw(u,clear)); u.visibility=1;
    CHECK(t.Draw(u,a)); u.time=95; CHECK(t.Draw(u,b)); CHECK(Differences(a,b)==0); // No gratuitous motion during playback or pause.
    CHECK(Differences(a,clear)>1000 && a[0]==clear[0]);
    const auto cx=static_cast<unsigned>(u.center_x), cy=static_cast<unsigned>(u.center_y-12*u.scale);
    CHECK(Pixel(a,cx,cy)>220); // Play triangle covers centre.
    c=a; u.freeze=0.5f; CHECK(t.Draw(u,b)); CHECK(Differences(b,c)>50);
    u.freeze=0; CHECK(t.Draw(u,a)); CHECK(Pixel(a,cx,cy)<100 && Pixel(a,cx-4,cy)>220 && Pixel(a,cx+4,cy)>220);
    u.pointer_x=100; u.pointer_y=u.timeline_y; u.hover=1; u.timeline_hover=1; u.scrub_offset=0.17f; u.scrub_velocity=1.2f;
    CHECK(t.Draw(u,b)); CHECK(Differences(a,b)>1000);
    u.hover=u.timeline_hover=u.scrub_offset=u.scrub_velocity=0;
    CHECK(t.Draw(u,a,0.02f,5000000)); CHECK(t.Draw(u,b,0.02f,-1)); CHECK(Differences(a,b)>500);
    CHECK(t.Draw(u,c,0.02f,60000000)); CHECK(Differences(b,c)==0); // Exact end boundary removes captions.
    CHECK(t.Draw(u,c,0.02f,5000000)); CHECK(Differences(a,c)==0); // Backward seek restores identical shaped text.
    // White frames remain dark behind the controls; text outline provides an edge on white.
    CHECK(t.Draw(u,b,1.0f,25000000)); CHECK(Pixel(b,cx,cy+16)<80);
    {   // No seam: on white, the shadow has decayed to the background at the scissor's last column and row.
        using media::render::HudPipeline;
        const auto left=static_cast<unsigned>(u.center_x-(u.dock_half_width+HudPipeline::kHorizontalMargin)*u.scale);
        const auto right=static_cast<unsigned>(u.center_x+(u.dock_half_width+HudPipeline::kHorizontalMargin)*u.scale)-1;
        const auto top=static_cast<unsigned>(u.center_y-(u.dock_half_height+HudPipeline::kVerticalMargin)*u.scale);
        const auto shadow_y=static_cast<unsigned>(u.center_y+8*u.scale);
        const auto top_x=static_cast<unsigned>(u.center_x+(u.dock_half_width-40)*u.scale); // Clear of the centred captions.
        std::printf("Dock edge on white: left=%u right=%u top=%u, inside shadow=%u\n",Pixel(b,left,shadow_y),Pixel(b,right,shadow_y),
                    Pixel(b,top_x,top),Pixel(b,static_cast<unsigned>(u.center_x+(u.dock_half_width+12)*u.scale),shadow_y));
        CHECK(Pixel(b,left,shadow_y)==255 && Pixel(b,right,shadow_y)==255 && Pixel(b,top_x,top)==255);
        CHECK(Pixel(b,static_cast<unsigned>(u.center_x+(u.dock_half_width+12)*u.scale),shadow_y)<250); // The shadow exists.
    }
    {   // Volume pill, in the dock between the transport button and the duration clock.
        auto v=u; v.hover=v.timeline_hover=v.scrub_offset=v.scrub_velocity=0;
        using media::ui::KineticHud;
        const float left=v.center_x+(v.volume_x-v.volume_half_width)*v.scale;
        const float from=left+KineticHud::kVolumeTrackLeft*v.scale, to=v.center_x+(v.volume_x+v.volume_half_width-KineticHud::kVolumeTrackRight)*v.scale;
        const auto y=static_cast<unsigned>(v.center_y+KineticHud::kVolumeY*v.scale);
        const auto at=[&](float level) { return static_cast<unsigned>(from+level*(to-from)); };
        const auto glyph=static_cast<unsigned>(left+25.5f*v.scale); // Where the mute cross's strokes meet; between the two waves.
        CHECK(t.Draw(v,clear)); v.volume=0.3f; v.muted=1; CHECK(t.Draw(v,c)); CHECK(Differences(clear,c)==0); // Faded out: nothing of it is drawn.
        v.volume_visibility=1; v.muted=0; CHECK(t.Draw(v,c)); CHECK(Differences(clear,c)>300);
        CHECK(Pixel(c,at(0.15f),y)>150 && Pixel(c,at(0.6f),y)<110 && Pixel(c,glyph,y)<110); // Filled up to 30 %, not beyond.
        const auto dim=Pixel(c,at(0.15f),y);
        v.volume=0.8f; CHECK(t.Draw(v,c)); CHECK(Pixel(c,at(0.6f),y)>150 && Pixel(c,at(0.9f),y)<110);
        v.time=95; CHECK(t.Draw(v,clear)); CHECK(Differences(clear,c)==0); // Still: no motion of its own.
        v.muted=1; CHECK(t.Draw(v,c)); CHECK(Pixel(c,glyph,y)>200 && Differences(clear,c)>60); // The mute cross, in place of the waves.
        CHECK(Pixel(c,at(0.15f),y)<dim); // The level is kept but dimmed while muted.
        v.volume_visibility=0.5f; CHECK(t.Draw(v,clear)); CHECK(Pixel(clear,glyph,y)<Pixel(c,glyph,y) && Pixel(clear,glyph,y)>60); // Half faded.
        std::printf("Volume pill: fill %u / track %u, mute cross %u (half faded %u)\n",dim,Pixel(c,at(0.9f),y),Pixel(c,glyph,y),Pixel(clear,glyph,y));
    }
    {   // Chapter ticks, scene dots and the scrub preview cue (docs/chapters.md; docs/seeking.md, "Scrub preview").
        auto v=u; v.hover=v.timeline_hover=v.scrub_offset=v.scrub_velocity=0; v.progress=0.2f;
        const float half=(v.dock_half_width-28)*v.scale, track_y=v.center_y+v.timeline_y*v.scale;
        const auto x_at=[&](double f) { return static_cast<unsigned>(v.center_x-half+2*half*static_cast<float>(f)+0.5f); };
        const auto y_at=[&](float design) { return static_cast<unsigned>(track_y+design*v.scale+0.5f); };
        const auto blue=[](const std::array<std::byte,Bytes>& p, unsigned x, unsigned y) { return static_cast<unsigned>(p[(y*Width+x)*4+2]); };
        media::ui::TimelineMarks marks; CHECK(marks.Add(0.5,true) && marks.Add(0.75,false));
        CHECK(t.Draw(v,a)); CHECK(t.Draw(v,b,0.15f,-1,marks)); CHECK(t.Draw(v,c,0.15f,-1,{}));
        CHECK(Differences(a,c)==0); // No marks: the dock as before.
        const auto tick=Pixel(b,x_at(0.5),y_at(-4)), beside=Pixel(b,x_at(0.5)+4,y_at(-4)), dot=Pixel(b,x_at(0.75),y_at(-6)), bare=Pixel(a,x_at(0.75),y_at(-6));
        CHECK(tick>Pixel(a,x_at(0.5),y_at(-4))+60 && beside<tick-60);    // A thin tick across the track at the chapter.
        CHECK(dot>bare+30 && Differences(a,b)<400);                      // A small dot above it at the scene; nothing else.
        v.scrub_offset=0.2f; CHECK(t.Draw(v,a)); v.preview=1; CHECK(t.Draw(v,c));
        const auto knob_x=x_at(0.4), head_x=x_at(0.2), mid=y_at(0);
        CHECK(Pixel(c,knob_x,mid)+50<Pixel(a,knob_x,mid));               // The knob is a ring while previewing.
        CHECK(Pixel(c,head_x,mid)>blue(c,head_x,mid)+80 && Pixel(a,head_x,mid)<=blue(a,head_x,mid)+40); // An amber diamond where the picture is.
        std::printf("Timeline marks: tick %u (beside %u), scene dot %u (bare %u); preview knob centre %u (exact %u), diamond R/B %u/%u\n", tick, beside, dot, bare,
                    Pixel(c,knob_x,mid), Pixel(a,knob_x,mid), Pixel(c,head_x,mid), blue(c,head_x,mid));
    }
    const auto allocations=allocation_probe::End(); CHECK(allocations==0);
    if (t.messages) CHECK(t.messages->GetNumStoredMessages()==0);
    if (argc>1) {
        Save(argv[1],"dock-dark",a); Save(argv[1],"dock-bright-rtl",b);
        CHECK(t.Draw(u,c,0.08f,45000000)); Save(argv[1],"dock-multilingual",c);
        u.timeline_hover=u.hover=1; u.pointer_x=100; u.pointer_y=u.timeline_y; u.scrub_offset=0.17f;
        CHECK(t.Draw(u,c,0.20f,5000000)); Save(argv[1],"dock-scrubbing",c);
        u.timeline_hover=u.hover=u.scrub_offset=0; u.volume_visibility=1; u.volume=0.65f;
        CHECK(t.Draw(u,c,0.20f,5000000)); Save(argv[1],"dock-volume",c);
        u.muted=1; CHECK(t.Draw(u,c,1.0f,25000000)); Save(argv[1],"dock-muted",c); u.muted=u.volume_visibility=0;
        media::ui::TimelineMarks marks; for (const double f : {0.12, 0.31, 0.55, 0.83}) (void)marks.Add(f, true);
        for (const double f : {0.2, 0.25, 0.44, 0.68, 0.74}) (void)marks.Add(f, false);
        u.timeline_hover=1; u.scrub_offset=0.21f; u.preview=1;
        CHECK(t.Draw(u,c,0.20f,-1,marks)); Save(argv[1],"dock-chapters-preview",c); u.timeline_hover=u.scrub_offset=u.preview=0;
    }
    Test compact; CHECK(SUCCEEDED(compact.Prepare(640,360)));
    if (!failures) {
        model.Configure(640,360,37250000000ll,true); auto s=model.Uniforms(); s.freeze=1; s.energy=0; s.progress=0.38f; s.elapsed_seconds=1415;
        CHECK(compact.Draw(s,c,0.1f,5000000)); if (argc>1) Save(argv[1],"dock-small",c,640,360);
    }
    std::printf("Glass dock / SDF subtitles GPU: %d failures, %llu allocations\n",failures,static_cast<unsigned long long>(allocations)); return failures ? 1 : 0;
}
