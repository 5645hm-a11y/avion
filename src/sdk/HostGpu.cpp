// Hardware decoding in host mode (API 14; src/sdk/HostGpu.hpp, docs/sdk.md "Hardware decoding in host mode"), and texture
// output's C structs.
#include "HostGpu.hpp"
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::sdk {
namespace {
template <typename T> void Drop(T*& object) noexcept { if (object) { object->Release(); object = nullptr; } }
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) noexcept {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return b;
}
constexpr std::uint32_t kConsumerTimeline = 2; // DecoderOptions::consumer_timeline_id.
}

render::SharedOutput::Format OutputFormat(std::uint32_t f) noexcept {
    using F = render::SharedOutput::Format;
    return f == MEDIA_OUTPUT_FORMAT_PQ10 ? F::Pq10 : f == MEDIA_OUTPUT_FORMAT_SCRGB16 ? F::Scrgb16 : F::Rgba8;
}
void FillTextures(const render::SharedOutput::Desc& d, render::SharedOutput::Format format, media_output_textures_t* out) noexcept {
    media_output_textures_t whole{};
    whole.struct_size = out->struct_size; whole.count = render::SharedOutput::kCount; whole.width = d.width; whole.height = d.height;
    whole.format = static_cast<uint32_t>(d.format); whole.generation = d.generation;
    for (unsigned i = 0; i != render::SharedOutput::kCount; ++i) whole.textures[i] = d.textures[i];
    whole.fence = d.fence; whole.host_fence = d.host_fence; whole.adapter_luid_low = d.adapter.LowPart; whole.adapter_luid_high = d.adapter.HighPart;
    whole.color_space = render::SharedOutput::ColorSpace(format);
    whole.sdr_white_nits = format == render::SharedOutput::Format::Rgba8 ? 0.0f : render::SharedOutput::kSdrWhiteNits;
    std::memcpy(out, &whole, out->struct_size);
}
void FillFrame(const render::SharedOutput::Frame& f, media_output_frame_t* out) noexcept {
    media_output_frame_t whole{};
    whole.struct_size = out->struct_size; whole.index = f.index; whole.fence_value = f.fence_value; whole.pts_100ns = f.pts;
    whole.generation = f.generation; whole.sequence = f.sequence; whole.transfer = f.transfer;
    std::memcpy(out, &whole, out->struct_size);
}

bool HostGpuVideo::CreateDevice() noexcept {
    if (device_) return true;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(hr)) hr = device_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_));
    if (SUCCEEDED(hr)) hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (SUCCEEDED(hr)) hr = color_.Create(device_);
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kRing * 2 + render::SharedOutput::kCount; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(hr)) hr = device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_));
    if (SUCCEEDED(hr)) stride_ = device_->GetDescriptorHandleIncrementSize(heap.Type);
    if (SUCCEEDED(hr) && !output_.CreateFences(device_)) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        // The format the host asked for must be a typed UAV store target here (the conversion writes it directly).
        D3D12_FEATURE_DATA_FORMAT_SUPPORT f{render::SharedOutput::Dxgi(format_), D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        hr = device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof(f));
        if (SUCCEEDED(hr) && !(f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) hr = DXGI_ERROR_UNSUPPORTED;
    }
    for (auto& t : targets_) {
        if (SUCCEEDED(hr)) hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&t.allocator));
        if (SUCCEEDED(hr)) hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, t.allocator, nullptr, IID_PPV_ARGS(&t.list));
        if (SUCCEEDED(hr)) hr = t.list->Close();
    }
    if (!event_) event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !event_) { Release(); return false; }
    return true;
}
bool HostGpuVideo::CreateTargets(unsigned width, unsigned height) noexcept {
    // Written as a UAV; Direct3D 11 opens such a shared texture only with simultaneous access (without it OpenSharedResource1 says
    // E_INVALIDARG; measured), so the textures are made that way (render target too, as the windowed engine's).
    if (!output_.CreateTextures(device_, width, height, format_, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) return false;
    for (unsigned i = 0; i != render::SharedOutput::kCount; ++i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = render::SharedOutput::Dxgi(format_); uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(output_.Texture(i), nullptr, &uav, Cpu(kRing * 2 + i));
        targets_[i].fence = 0;
    }
    return true;
}
D3D12_CPU_DESCRIPTOR_HANDLE HostGpuVideo::Cpu(unsigned i) const noexcept {
    auto h = heap_->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * stride_; return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE HostGpuVideo::Gpu(unsigned i) const noexcept {
    auto h = heap_->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(i) * stride_; return h;
}
bool HostGpuVideo::Idle(unsigned milliseconds) noexcept {
    if (!queue_ || !fence_) return true;
    const auto value = ++value_;
    if (FAILED(queue_->Signal(fence_, value))) return false;
    if (fence_->GetCompletedValue() >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, event_))) return false;
    return WaitForSingleObject(event_, milliseconds) == WAIT_OBJECT_0;
}
bool HostGpuVideo::PrepareDecode(core::LinearMemoryArena& arena, const demux::TrackInfo& track, unsigned coded_width, unsigned coded_height,
                                 unsigned crop_height, bool ten_bit, unsigned reorder) noexcept {
    using platform::windows::DecodeStatus;
    CloseDecode();
    // A stream that reorders more pictures than the ring holds would wait forever for one to show: the fallback decodes it.
    if (!width_ || !height_ || reorder + 2 > kRing || coded_width > 8192 || coded_height > 8192) return false;
    if (!CreateDevice()) return false;
    platform::windows::DecoderOptions o;
    o.codec = track.codec == demux::Codec::Hevc ? platform::windows::VideoCodec::Hevc : platform::windows::VideoCodec::H264;
    o.width = static_cast<std::uint16_t>(coded_width); o.height = static_cast<std::uint16_t>(coded_height);
    o.surfaces = kSurfaces; o.ten_bit = ten_bit; o.decode_timeline_id = 1; o.consumer_timeline_id = kConsumerTimeline;
    // Upload capacity: the largest access unit (Annex B start codes and alignment added), as the windowed engine sizes it.
    const std::uint64_t sample = track.max_sample_bytes ? track.max_sample_bytes : 8ull << 20;
    o.max_bitstream_bytes = static_cast<std::uint32_t>((std::max)((sample + 3 * 256 + 255) & ~std::uint64_t{127}, std::uint64_t{65536}));
    o.epoch = 1;
    if (decoder_.Prepare(arena, device_, fence_, o) != DecodeStatus::Ready) { (void)decoder_.Close(); return false; }
    // The picture ring: the decoder's surface description (its height alignment included), each with its copy's list and views.
    const auto desc = decoder_.SurfaceAt(0)->GetDesc();
    D3D12_HEAP_PROPERTIES local{}; local.Type = D3D12_HEAP_TYPE_DEFAULT; local.CreationNodeMask = local.VisibleNodeMask = 1;
    HRESULT hr = S_OK;
    for (unsigned i = 0; i != kRing && SUCCEEDED(hr); ++i) {
        auto& p = ring_[i];
        hr = device_->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.texture));
        if (SUCCEEDED(hr)) hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p.allocator));
        if (SUCCEEDED(hr)) hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.allocator, nullptr, IID_PPV_ARGS(&p.list));
        if (SUCCEEDED(hr)) hr = p.list->Close();
        for (unsigned plane = 0; plane != 2 && SUCCEEDED(hr); ++plane) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = ten_bit ? (plane ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM) : (plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM);
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
            device_->CreateShaderResourceView(p.texture, &srv, Cpu(i * 2 + plane));
        }
        p.fence = 0; p.waiting = false; p.owner = {};
    }
    // The shared textures: made once, again only for another size or format (the host keeps its opened ones meanwhile).
    if (SUCCEEDED(hr) && (output_.Width() != width_ || output_.Height() != height_ || output_.TextureFormat() != format_ || !output_.Texture(0)) &&
        (!Idle(2000) || (output_.ReleaseTextures(), !CreateTargets(width_, height_)))) hr = E_FAIL;
    if (FAILED(hr)) { prepared_ = true; CloseDecode(); return false; }
    container_ = {static_cast<std::uint8_t>(track.color.primaries > 255 ? video::kUnspecified : track.color.primaries),
                  static_cast<std::uint8_t>(track.color.transfer > 255 ? video::kUnspecified : track.color.transfer),
                  static_cast<std::uint8_t>(track.color.matrix > 255 ? video::kUnspecified : track.color.matrix),
                  track.color.full_range, track.color.described, track.color.range_described};
    crop_height_ = crop_height; ten_bit_ = ten_bit; peak_.Restart(); stats_ = {};
    prepared_ = true;
    return true;
}
int HostGpuVideo::FreePicture() const noexcept {
    const auto done = fence_ ? fence_->GetCompletedValue() : 0;
    if (done == UINT64_MAX) return -1;
    for (unsigned i = 0; i != kRing; ++i) if (!ring_[i].waiting && ring_[i].texture && done >= ring_[i].fence) return static_cast<int>(i);
    return -1;
}
bool HostGpuVideo::Ready() const noexcept { return prepared_ && decoder_.CanReserve() && FreePicture() >= 0; }
engine::HostGpu::Status HostGpuVideo::Decode(const codec::H264Picture* h264, const codec::HevcPicture* hevc, const core::MediaPacket& packet,
                                             const platform::BlockView& view, bool display, const engine::HostGpuSlot* slots, unsigned slot_count,
                                             platform::SurfaceHandle& surface) noexcept {
    using platform::windows::DecodeStatus;
    if (!prepared_ || (!h264 && !hevc)) return Status::Failed;
    const int slot = display ? FreePicture() : -1;
    if (display && slot < 0) return Status::Busy;
    auto native = h264 ? h264_dxva_.Prepare(*h264, packet, view) : hevc_dxva_.Prepare(*hevc, packet, view);
    // The references, named by the engine's slots, as the decoder's surfaces (native.references views this same array).
    auto* references = h264 ? h264_dxva_.references : hevc_dxva_.references;
    for (std::size_t i = 0; i < native.references.size(); ++i) {
        const auto r = references[i];
        if (r.id >= slot_count || !slots[r.id].valid || slots[r.id].generation != r.generation) { ++stats_.failed; return Status::Failed; }
        references[i] = slots[r.id].surface;
    }
    native.display = display;
    native.color_metadata_id = video::PackColor(video::ResolveColor(container_, video::UnpackColor(native.color_metadata_id), crop_height_));
    platform::SurfaceHandle submitted{};
    const auto status = decoder_.Submit(native, frames_, returns_, &submitted);
    core::VideoFrameDescriptor frame{};
    if (const auto* f = frames_.PeekReadSlot()) { frame = *f; (void)frames_.ReleaseReadSlot(); }
    if (returns_.PeekReadSlot()) (void)returns_.ReleaseReadSlot(); // The packet's bytes are no longer read (CPU packing done).
    if (status == DecodeStatus::WouldBlock) return Status::Busy;
    if (status != DecodeStatus::Submitted) { ++stats_.failed; return Status::Failed; }
    ++stats_.decoded;
    surface = submitted;
    if (!display) {
        // A reference only: nothing reads it but the decoder (returned at once, unread).
        (void)decoder_.ReturnSurface({submitted.id, submitted.generation, frame.playback_epoch, kConsumerTimeline, 0});
        return Status::Submitted;
    }
    // A picture to show: copied (on the GPU, once decoded) into the ring, and the decoder's surface returned at the copy's value.
    auto& p = ring_[slot];
    auto* source = decoder_.SurfaceAt(submitted.id);
    HRESULT hr = p.allocator->Reset();
    if (SUCCEEDED(hr)) hr = p.list->Reset(p.allocator, nullptr);
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER b[2]{Transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                    Transition(p.texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
        p.list->ResourceBarrier(2, b);
        p.list->CopyResource(p.texture, source);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        p.list->ResourceBarrier(2, b);
        hr = p.list->Close();
    }
    if (SUCCEEDED(hr)) hr = queue_->Wait(decoder_.DecodeFence(), frame.ready_fence_value);
    if (SUCCEEDED(hr)) { ID3D12CommandList* lists[]{p.list}; queue_->ExecuteCommandLists(1, lists); }
    const auto value = ++value_;
    if (SUCCEEDED(hr)) hr = queue_->Signal(fence_, value);
    // Returned whatever happened: a failed copy leaves nothing to show (the picture is still the decoder's reference if it is one).
    (void)decoder_.ReturnSurface({submitted.id, submitted.generation, frame.playback_epoch, kConsumerTimeline, SUCCEEDED(hr) ? value : 0});
    if (FAILED(hr)) { ++stats_.failed; surface = submitted; return Status::Submitted; } // Shown as nothing (Show finds no picture).
    p.fence = value; p.owner = submitted; p.frame = frame; p.waiting = true;
    ++stats_.copied;
    return Status::Submitted;
}
void HostGpuVideo::Discard(platform::SurfaceHandle surface) noexcept {
    for (auto& p : ring_) if (p.waiting && p.owner.id == surface.id && p.owner.generation == surface.generation) { p.waiting = false; ++stats_.discarded; return; }
}
void HostGpuVideo::ApplySize() noexcept {
    const auto asked = size_request_.exchange(0, std::memory_order_acq_rel);
    if (!asked) return;
    const unsigned width = static_cast<unsigned>(asked >> 32), height = static_cast<unsigned>(asked & 0xffffffffu);
    if (width == output_.Width() && height == output_.Height()) return;
    // A new generation: the textures' GPU work done first (a resize is not the steady state), then new ones of the new size.
    if (!Idle(2000)) return;
    output_.ReleaseTextures();
    width_ = width; height_ = height;
    (void)CreateTargets(width, height);
}
bool HostGpuVideo::Show(platform::SurfaceHandle surface, const engine::HostGpuShow& show) noexcept {
    if (!prepared_) return false;
    Picture* picture = nullptr;
    for (auto& p : ring_) if (p.waiting && p.owner.id == surface.id && p.owner.generation == surface.generation) picture = &p;
    if (!picture) return false;
    picture->waiting = false;
    ApplySize();
    if (!output_.Texture(0)) return false;
    const auto done = fence_->GetCompletedValue();
    if (done == UINT64_MAX) return false;
    const int index = output_.Claim([&](unsigned i) { return done >= targets_[i].fence; });
    if (index < 0) { ++stats_.no_texture; return false; } // The host holds two textures: this picture is not drawn.
    auto& t = targets_[index];
    auto* target = output_.Texture(static_cast<unsigned>(index));
    const auto& f = picture->frame;
    const unsigned W = output_.Width(), H = output_.Height();
    // The view's region of the crop (zoom about its centre, kept inside), on even samples for 4:2:0.
    const unsigned cl = f.crop_left, ct = f.crop_top, cw = f.crop_right - f.crop_left, ch = f.crop_bottom - f.crop_top;
    const double zoom = (std::max)(1.0, static_cast<double>(show.view_zoom));
    const double rw = cw / zoom, rh = ch / zoom;
    const double x0 = (std::clamp)(cl + show.view_x * cw - rw / 2, static_cast<double>(cl), cl + cw - rw);
    const double y0 = (std::clamp)(ct + show.view_y * ch - rh / 2, static_cast<double>(ct), ct + ch - rh);
    unsigned left = static_cast<unsigned>(x0) & ~1u, top = static_cast<unsigned>(y0) & ~1u;
    unsigned right = (std::min)(f.crop_right + 0u, left + ((static_cast<unsigned>(rw + 0.5) + 1u) & ~1u));
    unsigned bottom = (std::min)(f.crop_bottom + 0u, top + ((static_cast<unsigned>(rh + 0.5) + 1u) & ~1u));
    if (right <= left || bottom <= top) { left = cl; top = ct; right = f.crop_right; bottom = f.crop_bottom; }
    const double region_w = right - left, region_h = bottom - top;
    // Fitted, centred, black around it (kRotate's mapping with kBars): q = X / s - o / s, luma texels from the region's origin.
    const double s = (std::min)(W / region_w, H / region_h);
    const double ox = (W - region_w * s) / 2, oy = (H - region_h * s) / 2;
    const std::uint32_t transfer = (f.color_metadata_id >> 16) & 255u;
    std::uint32_t peak_flags = 0;
    const float peak_dt = render::IsHdrTransfer(transfer) ? peak_.Step(show.pts_100ns, peak_flags) : 0.0f;
    render::ColorConstants c{};
    c.width = W; c.height = H; c.coded_width = f.coded_width; c.coded_height = f.coded_height;
    c.left = left; c.top = top; c.right = right; c.bottom = bottom;
    c.matrix = f.color_metadata_id & 255u; c.full_range = (f.color_metadata_id >> 8) & 1u; c.ten_bit = ten_bit_ ? 1u : 0u; c.primaries = f.color_metadata_id >> 24;
    c.transfer = transfer; c.peak_mode = 0; c.fixed_peak_nits = 1000.0f; c.target_nits = render::SharedOutput::kSdrWhiteNits;
    c.reconstruction = render::kEdgeDirected; c.sharpness = 0.0f; c.noise_floor = 0.02f; c.chroma_mode = render::kGuidedChroma;
    c.peak_dt = peak_dt; c.peak_flags = peak_flags;
    c.filter_flags = render::kAreaPrefilter | render::kRotate | render::kBars;
    if (format_ == render::SharedOutput::Format::Rgba8) c.filter_flags |= render::kDither;
    else c.filter_flags |= render::kHdrOutput | (format_ == render::SharedOutput::Format::Scrgb16 ? render::kLinearOutput : 0u);
    c.contrast = c.saturation = c.gamma = c.micro_gain = 1.0f;
    c.view_m[0] = static_cast<float>(1 / s); c.view_m[3] = static_cast<float>(1 / s);
    c.view_t[0] = static_cast<float>(-ox / s); c.view_t[1] = static_cast<float>(-oy / s);
    HRESULT hr = t.allocator->Reset();
    if (SUCCEEDED(hr)) hr = t.list->Reset(t.allocator, nullptr);
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER b[2]{Transition(picture->texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                    Transition(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        t.list->ResourceBarrier(2, b);
        ID3D12DescriptorHeap* heaps[]{heap_}; t.list->SetDescriptorHeaps(1, heaps);
        const unsigned slot = static_cast<unsigned>(picture - ring_);
        color_.Record(t.list, Gpu(slot * 2), Gpu(kRing * 2 + static_cast<unsigned>(index)), c);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        t.list->ResourceBarrier(2, b);
        hr = t.list->Close();
    }
    // The host's reads of this texture first (its release value), on the GPU.
    if (SUCCEEDED(hr)) if (const auto wait = output_.HostWait(static_cast<unsigned>(index)); wait) hr = queue_->Wait(output_.HostFence(), wait);
    if (FAILED(hr)) { output_.Unclaim(static_cast<unsigned>(index)); ++stats_.failed; return false; }
    ID3D12CommandList* lists[]{t.list}; queue_->ExecuteCommandLists(1, lists);
    const auto value = ++value_;
    if (FAILED(queue_->Signal(fence_, value))) { output_.Unclaim(static_cast<unsigned>(index)); ++stats_.failed; return false; }
    t.fence = value; picture->fence = value;
    if (!output_.Publish(queue_, static_cast<unsigned>(index), show.pts_100ns, transfer)) { ++stats_.failed; return false; }
    ++stats_.shown;
    return true;
}
bool HostGpuVideo::Restart(std::uint32_t epoch) noexcept {
    if (!prepared_) return false;
    for (auto& p : ring_) p.waiting = false; // Each was discarded already; a picture of the earlier epoch is never shown.
    peak_.Restart();
    return decoder_.SetEpoch(epoch);
}
void HostGpuVideo::CloseDecode() noexcept {
    if (!prepared_) return;
    (void)Idle(2000);
    (void)decoder_.Close();
    for (auto& p : ring_) { Drop(p.list); Drop(p.allocator); Drop(p.texture); p.fence = 0; p.waiting = false; p.owner = {}; }
    while (frames_.PeekReadSlot()) (void)frames_.ReleaseReadSlot();
    while (returns_.PeekReadSlot()) (void)returns_.ReleaseReadSlot();
    prepared_ = false;
}
void HostGpuVideo::Release() noexcept {
    CloseDecode();
    (void)Idle(2000);
    output_.Release();
    for (auto& t : targets_) { Drop(t.list); Drop(t.allocator); t.fence = 0; }
    color_.Release();
    Drop(heap_); Drop(fence_); Drop(queue_); Drop(device_);
    if (event_) { CloseHandle(event_); event_ = nullptr; }
    value_ = 0;
}
}
