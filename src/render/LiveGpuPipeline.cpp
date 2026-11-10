#include "media/render/LiveGpuPipeline.hpp"
#include "media/render/NeuralWeights.hpp"
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::render {
namespace {
template <class T> void ReleaseCom(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) noexcept {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to}; return b;
}
HRESULT Buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state, ID3D12Resource** out) noexcept {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(out));
}
HRESULT Texture(ID3D12Device* device, unsigned width, unsigned height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, ID3D12Resource** out) noexcept {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = flags;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(out));
}
}
D3D12_CPU_DESCRIPTOR_HANDLE LiveGpuPipeline::Cpu(unsigned i) const noexcept { auto h = heap_->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * stride_; return h; }
D3D12_GPU_DESCRIPTOR_HANDLE LiveGpuPipeline::Gpu(unsigned i) const noexcept { auto h = heap_->GetGPUDescriptorHandleForHeapStart(); h.ptr += static_cast<UINT64>(i) * stride_; return h; }
bool LiveGpuPipeline::Create(unsigned width, unsigned height, unsigned out_width, unsigned out_height, bool ten_bit) noexcept {
    Release();
    if (width < 64 || height < 64 || width > 4096 || height > 4096 || (width | height) & 1 || out_width < 16 || out_height < 16 ||
        out_width > 8192 || out_height > 8192 || (out_width | out_height) & 1) { error_ = E_INVALIDARG; return false; }
    width_ = width; height_ = height; out_width_ = out_width; out_height_ = out_height; ten_bit_ = ten_bit;
    IDXGIFactory6* factory = nullptr;
    if (SUCCEEDED(error_ = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        for (UINT i = 0; !device_; ++i) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                (void)D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
            ReleaseCom(adapter);
        }
    }
    ReleaseCom(factory);
    if (!device_) { error_ = DXGI_ERROR_UNSUPPORTED; return false; }
    const auto fail = [this](HRESULT hr) { error_ = hr; Release(); return false; };
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(error_ = device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_)))) return fail(error_);
    if (FAILED(error_ = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return fail(error_);
    if (!(wait_ = CreateEventW(nullptr, FALSE, FALSE, nullptr))) return fail(HRESULT_FROM_WIN32(GetLastError()));
    // Micro-motion's reference twice over: read from one half, written to the other (it follows the view).
    if (FAILED(error_ = color_.Create(device_)) || FAILED(error_ = color_.PrepareMotion(device_, 2 * std::uint64_t{out_width} * out_height))) return fail(error_);
    // Neural 2x luma of up to the whole picture; without it the conversion stays edge-directed.
    if (FAILED(neural_.Create(device_, queue_, kNeuralWeights, width, height)) ||
        FAILED(Texture(device_, 2 * width, 2 * height, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &neural_texture_))) {
        neural_.Release(); ReleaseCom(neural_texture_);
    }
    const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * kRing + 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(error_ = device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return fail(error_);
    stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (neural_texture_) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = DXGI_FORMAT_R16_FLOAT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(neural_texture_, nullptr, &uav, Cpu(3 * kRing));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R16_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(neural_texture_, &srv, Cpu(3 * kRing + 1));
    }
    for (unsigned k = 0; k < kRing; ++k) {
        auto& s = slots_[k];
        if (FAILED(error_ = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
            FAILED(error_ = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr, IID_PPV_ARGS(&s.list))) ||
            FAILED(error_ = s.list->Close()) ||
            FAILED(error_ = Texture(device_, width, height, ten_bit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12, D3D12_RESOURCE_FLAG_NONE, &s.source)) ||
            FAILED(error_ = Texture(device_, out_width, out_height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &s.image)))
            return fail(error_);
        if (!k) {
            const auto source = s.source->GetDesc(), image = s.image->GetDesc();
            UINT rows[2]{}; UINT64 sizes[2]{};
            device_->GetCopyableFootprints(&source, 0, 2, 0, planes_, rows, sizes, nullptr);
            device_->GetCopyableFootprints(&image, 0, 1, 0, &image_footprint_, nullptr, nullptr, nullptr);
        }
        UINT64 upload_bytes = 0, image_bytes = 0;
        const auto source = s.source->GetDesc(), image = s.image->GetDesc();
        device_->GetCopyableFootprints(&source, 0, 2, 0, nullptr, nullptr, nullptr, &upload_bytes);
        device_->GetCopyableFootprints(&image, 0, 1, 0, nullptr, nullptr, nullptr, &image_bytes);
        if (FAILED(error_ = Buffer(device_, D3D12_HEAP_TYPE_UPLOAD, upload_bytes, D3D12_RESOURCE_STATE_GENERIC_READ, &s.upload)) ||
            FAILED(error_ = Buffer(device_, D3D12_HEAP_TYPE_READBACK, image_bytes, D3D12_RESOURCE_STATE_COPY_DEST, &s.readback)) ||
            FAILED(error_ = s.upload->Map(0, nullptr, reinterpret_cast<void**>(&s.upload_data))) ||
            FAILED(error_ = s.readback->Map(0, nullptr, reinterpret_cast<void**>(&s.readback_data))))
            return fail(error_);
        for (unsigned plane = 0; plane < 2; ++plane) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = ten_bit ? (plane ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM) : (plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM);
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1; srv.Texture2D.PlaneSlice = plane;
            device_->CreateShaderResourceView(s.source, &srv, Cpu(3 * k + plane));
        }
        device_->CreateUnorderedAccessView(s.image, nullptr, nullptr, Cpu(3 * k + 2));
    }
    // GPU time per picture (optional): a timestamp before the upload and after the conversion.
    const D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kRing, 0};
    if (SUCCEEDED(queue_->GetTimestampFrequency(&ticks_)) && ticks_ && SUCCEEDED(device_->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamps_))) &&
        SUCCEEDED(Buffer(device_, D3D12_HEAP_TYPE_READBACK, 16 * kRing, D3D12_RESOURCE_STATE_COPY_DEST, &timestamp_readback_)) &&
        SUCCEEDED(timestamp_readback_->Map(0, nullptr, reinterpret_cast<void**>(&timestamp_data_)))) {}
    else { ReleaseCom(timestamps_); ReleaseCom(timestamp_readback_); timestamp_data_ = nullptr; }
    error_ = S_OK; micro_start_ = true; previous_valid_ = false; micro_phase_ = 0; latest_ = -1; next_ = 0; sequence_ = 0; stats_ = {};
    return true;
}
void LiveGpuPipeline::Release() noexcept {
    if (queue_ && fence_) (void)Drain(5000);
    for (auto& s : slots_) {
        if (s.readback && s.readback_data) { const D3D12_RANGE none{0, 0}; s.readback->Unmap(0, &none); }
        if (s.upload && s.upload_data) s.upload->Unmap(0, nullptr);
        ReleaseCom(s.readback); ReleaseCom(s.upload); ReleaseCom(s.image); ReleaseCom(s.source); ReleaseCom(s.list); ReleaseCom(s.allocator);
        s = Slot{};
    }
    if (timestamp_readback_ && timestamp_data_) { const D3D12_RANGE none{0, 0}; timestamp_readback_->Unmap(0, &none); }
    timestamp_data_ = nullptr;
    ReleaseCom(timestamp_readback_); ReleaseCom(timestamps_); ReleaseCom(neural_texture_); ReleaseCom(heap_);
    neural_.Release(); color_.Release();
    if (wait_) { CloseHandle(wait_); wait_ = nullptr; }
    ReleaseCom(fence_); ReleaseCom(queue_); ReleaseCom(device_);
    latest_ = -1; ten_bit_ = false;
}
void LiveGpuPipeline::SetControls(const LiveControls& wanted) noexcept {
    const auto clamp = [](float v, float lo, float hi, float neutral) { return v == v ? (std::clamp)(v, lo, hi) : neutral; }; // NaN: neutral.
    LiveControls c{clamp(wanted.brightness, -0.5f, 0.5f, 0), clamp(wanted.contrast, 0, 2, 1), clamp(wanted.saturation, 0, 2, 1),
                   clamp(wanted.gamma, 0.5f, 2.5f, 1), clamp(wanted.shadow_lift, 0, 1, 0), clamp(wanted.micro_motion, 1, 8, 1),
                   (std::min)(wanted.reconstruction, std::uint32_t{kNeural})};
    if (c.micro_motion > 1 && controls_.micro_motion <= 1) micro_start_ = true;
    controls_ = c;
}
void LiveGpuPipeline::Region(const LiveView& v, std::uint32_t& left, std::uint32_t& top, std::uint32_t& right, std::uint32_t& bottom) const noexcept {
    // The windowed engine's zoom region (D3D12Renderer::RegionAt): the picture's shape at 1/zoom, on even samples, at least 16
    // of them, centred where asked, inside the picture.
    const float zoom = (std::max)(v.zoom == v.zoom ? v.zoom : 1.0f, 1.0f);
    const auto even = [](double x) { return static_cast<unsigned>(std::lround(x / 2)) * 2u; };
    unsigned rw = (std::clamp)(even(width_ / zoom), 16u, width_), rh = (std::clamp)(even(height_ / zoom), 16u, height_);
    if (v.display[0] == 0 && !v.fit) {
        // An odd quarter turn (2026-10-31): the region of the output's turned shape (out_height : out_width) as large as that
        // region holds, so that the turned picture fills the image undistorted. Fitted (2026-11-01): the view's own region.
        const double across = static_cast<double>(out_height_) / out_width_;
        if (static_cast<double>(rw) > rh * across) rw = (std::clamp)(even(rh * across), 16u, width_);
        else rh = (std::clamp)(even(rw / across), 16u, height_);
    }
    const double cx = static_cast<double>(v.x) * width_, cy = static_cast<double>(v.y) * height_;
    left = (std::min)(even((std::clamp)(cx - rw / 2.0, 0.0, static_cast<double>(width_ - rw))), width_ - rw);
    top = (std::min)(even((std::clamp)(cy - rh / 2.0, 0.0, static_cast<double>(height_ - rh))), height_ - rh);
    right = left + rw; bottom = top + rh;
}
void LiveGpuPipeline::Mapping::Forward(double x, double y, double& qx, double& qy) const noexcept {
    const double ux = (a[0] * x + a[1] * y + b[0]) * sx - cx, uy = (a[2] * x + a[3] * y + b[1]) * sy - cy;
    qx = left + cx + cos * ux - sin * uy + dx; qy = top + cy + sin * ux + cos * uy + dy;
}
void LiveGpuPipeline::Mapping::Inverse(double qx, double qy, double& x, double& y) const noexcept {
    const double vx = qx - (left + cx + dx), vy = qy - (top + cy + dy);
    const double ux = (cx + cos * vx + sin * vy) / sx - b[0], uy = (cy - sin * vx + cos * vy) / sy - b[1];
    x = a[0] * ux + a[2] * uy; y = a[1] * ux + a[3] * uy; // A is orthogonal: its inverse is its transpose.
}
void LiveGpuPipeline::Collect() noexcept {
    if (!fence_) return;
    const auto completed = fence_->GetCompletedValue();
    if (completed == UINT64_MAX) { error_ = DXGI_ERROR_DEVICE_REMOVED; return; }
    for (unsigned k = 0; k < kRing; ++k) {
        auto& s = slots_[k];
        if (!s.busy || completed < s.fence) continue;
        s.busy = false; ++stats_.completed;
        if (timestamp_data_) {
            const auto begin = timestamp_data_[2 * k], end = timestamp_data_[2 * k + 1];
            s.info.gpu_ms = end > begin ? static_cast<double>(end - begin) * 1000.0 / static_cast<double>(ticks_) : 0.0;
            stats_.gpu_ms_max = (std::max)(stats_.gpu_ms_max, s.info.gpu_ms);
        }
        if (latest_ < 0 || slots_[latest_].busy || s.fence > slots_[latest_].fence) latest_ = static_cast<int>(k);
    }
}
LiveGpuPipeline::Slot* LiveGpuPipeline::Begin() noexcept {
    if (!queue_ || FAILED(error_)) return nullptr;
    Collect();
    auto& s = slots_[next_];
    if (s.busy) { ++stats_.busy; return nullptr; }
    if (latest_ == static_cast<int>(next_)) latest_ = -1; // Its image is about to be overwritten.
    return &s;
}
LiveGpuPipeline::Status LiveGpuPipeline::Ingest(const std::uint8_t* luma, unsigned luma_stride, const std::uint8_t* cb, const std::uint8_t* cr,
                                                unsigned chroma_stride, std::int64_t pts, const LiveView& view) noexcept {
    if (!queue_ || !luma || !cb || !cr || FAILED(error_)) return Status::Failed;
    Slot* slot = Begin();
    if (!slot) return FAILED(error_) ? Status::Failed : Status::Busy;
    auto& s = *slot;
    // The picture into the slot's upload buffer as NV12 (interleaving the chroma), or as P010's 10-bit codes (v x 4, in the
    // high bits: v << 8).
    if (!ten_bit_) {
        for (unsigned y = 0; y < height_; ++y)
            std::memcpy(s.upload_data + planes_[0].Offset + static_cast<std::size_t>(y) * planes_[0].Footprint.RowPitch, luma + static_cast<std::size_t>(y) * luma_stride, width_);
        for (unsigned y = 0; y < height_ / 2; ++y) {
            auto* row = s.upload_data + planes_[1].Offset + static_cast<std::size_t>(y) * planes_[1].Footprint.RowPitch;
            const auto* u = cb + static_cast<std::size_t>(y) * chroma_stride; const auto* v = cr + static_cast<std::size_t>(y) * chroma_stride;
            for (unsigned x = 0; x < width_ / 2; ++x) { row[2 * x] = u[x]; row[2 * x + 1] = v[x]; }
        }
    } else {
        for (unsigned y = 0; y < height_; ++y) {
            auto* row = reinterpret_cast<std::uint16_t*>(s.upload_data + planes_[0].Offset + static_cast<std::size_t>(y) * planes_[0].Footprint.RowPitch);
            const auto* in = luma + static_cast<std::size_t>(y) * luma_stride;
            for (unsigned x = 0; x < width_; ++x) row[x] = static_cast<std::uint16_t>(in[x] << 8);
        }
        for (unsigned y = 0; y < height_ / 2; ++y) {
            auto* row = reinterpret_cast<std::uint16_t*>(s.upload_data + planes_[1].Offset + static_cast<std::size_t>(y) * planes_[1].Footprint.RowPitch);
            const auto* u = cb + static_cast<std::size_t>(y) * chroma_stride; const auto* v = cr + static_cast<std::size_t>(y) * chroma_stride;
            for (unsigned x = 0; x < width_ / 2; ++x) { row[2 * x] = static_cast<std::uint16_t>(u[x] << 8); row[2 * x + 1] = static_cast<std::uint16_t>(v[x] << 8); }
        }
    }
    return Submit(s, pts, view);
}
LiveGpuPipeline::Status LiveGpuPipeline::IngestP010(const std::uint8_t* luma, unsigned luma_stride, const std::uint8_t* cbcr, unsigned cbcr_stride,
                                                    std::int64_t pts, const LiveView& view) noexcept {
    if (!queue_ || !ten_bit_ || !luma || !cbcr || luma_stride < 2 * width_ || cbcr_stride < 2 * width_ || FAILED(error_)) return Status::Failed;
    Slot* slot = Begin();
    if (!slot) return FAILED(error_) ? Status::Failed : Status::Busy;
    auto& s = *slot;
    // Already the texture's layout: each row as it is (luma: width samples; CbCr: width / 2 pairs).
    for (unsigned y = 0; y < height_; ++y)
        std::memcpy(s.upload_data + planes_[0].Offset + static_cast<std::size_t>(y) * planes_[0].Footprint.RowPitch, luma + static_cast<std::size_t>(y) * luma_stride, 2u * width_);
    for (unsigned y = 0; y < height_ / 2; ++y)
        std::memcpy(s.upload_data + planes_[1].Offset + static_cast<std::size_t>(y) * planes_[1].Footprint.RowPitch, cbcr + static_cast<std::size_t>(y) * cbcr_stride, 2u * width_);
    return Submit(s, pts, view);
}
LiveGpuPipeline::Status LiveGpuPipeline::Submit(Slot& s, std::int64_t pts, const LiveView& view) noexcept {
    std::uint32_t left = 0, top = 0, right = 0, bottom = 0; Region(view, left, top, right, bottom);
    const unsigned rw = right - left, rh = bottom - top;
    // The view: the crop (even texels), the rest of its centre's position (at most a texel each way) and its turn.
    Mapping now{};
    now.left = left; now.top = top; now.cx = rw / 2.0; now.cy = rh / 2.0;
    // The display's turns and mirror (2026-10-31): output X -> the upright output U = A X + b, A = D's transpose, about the
    // centres; an odd turn's upright output is out_height x out_width.
    const int* d = view.display;
    const bool axes = ((d[0] && d[3] && !d[1] && !d[2]) || (!d[0] && !d[3] && d[1] && d[2])) && d[0] >= -1 && d[0] <= 1 && d[1] >= -1 && d[1] <= 1 &&
                      d[2] >= -1 && d[2] <= 1 && d[3] >= -1 && d[3] <= 1;
    const int shown[4]{axes ? d[0] : 1, axes ? d[1] : 0, axes ? d[2] : 0, axes ? d[3] : 1};
    const bool across = shown[0] == 0, upright = shown[0] == 1 && shown[3] == 1;
    const double uw = across ? out_height_ : out_width_, uh = across ? out_width_ : out_height_;
    now.a[0] = shown[0]; now.a[1] = shown[2]; now.a[2] = shown[1]; now.a[3] = shown[3];
    now.b[0] = uw / 2 - (now.a[0] * out_width_ / 2.0 + now.a[1] * out_height_ / 2.0);
    now.b[1] = uh / 2 - (now.a[2] * out_width_ / 2.0 + now.a[3] * out_height_ / 2.0);
    now.sx = static_cast<double>(rw) / uw; now.sy = static_cast<double>(rh) / uh;
    // Fit (2026-11-01): one scale both ways, the larger (the whole region inside the image), about the centres; where the turned
    // region does not reach, bars (kBars). The picture's rectangle in the image: the region's turned size at that scale, centred.
    bool bars = false;
    std::uint32_t inside[4]{0, 0, out_width_, out_height_};
    if (view.fit) {
        const double fit = (std::max)(static_cast<double>(rw) / uw, static_cast<double>(rh) / uh);
        now.sx = now.sy = fit;
        now.b[0] += rw / (2 * fit) - uw / 2; now.b[1] += rh / (2 * fit) - uh / 2;
        const double pw = (across ? rh : rw) / fit, ph = (across ? rw : rh) / fit; // The picture's size in the image (pixels).
        bars = pw < out_width_ - 0.5 || ph < out_height_ - 0.5;
        if (bars) {
            inside[0] = static_cast<std::uint32_t>(std::lround((out_width_ - pw) / 2)); inside[2] = static_cast<std::uint32_t>(std::lround((out_width_ + pw) / 2));
            inside[1] = static_cast<std::uint32_t>(std::lround((out_height_ - ph) / 2)); inside[3] = static_cast<std::uint32_t>(std::lround((out_height_ + ph) / 2));
        }
    }
    const float roll = view.roll == view.roll ? (std::clamp)(view.roll, -0.5f, 0.5f) : 0.0f;
    now.cos = std::cos(static_cast<double>(roll)); now.sin = std::sin(static_cast<double>(roll));
    if (view.zoom > 1.0f) {
        now.dx = (std::clamp)(static_cast<double>(view.x) * width_ - (left + now.cx), -1.0, 1.0);
        now.dy = (std::clamp)(static_cast<double>(view.y) * height_ - (top + now.cy), -1.0, 1.0);
    }
    const bool turned = roll != 0 || std::fabs(now.dx) > 1e-3 || std::fabs(now.dy) > 1e-3 || !upright || bars;
    const bool upscaling = out_width_ >= rw && out_height_ >= rh && (out_width_ > rw || out_height_ > rh);
    const bool neural = upscaling && !turned && controls_.reconstruction == kNeural && neural_texture_ && neural_.Ready();
    const std::uint32_t reconstruction = neural ? kNeural : controls_.reconstruction == kBilinear ? kBilinear : kEdgeDirected;
    // Camera pictures: BT.709 limited range, SDR, 8-bit or (ten-bit ring) 10-bit; dithered to the 8-bit image.
    ColorConstants c{out_width_, out_height_, width_, height_, left, top, right, bottom, 1u, 0u, ten_bit_ ? 1u : 0u, 1u, 1u, 0u, 1000.0f, 203.0f,
                     reconstruction, 0.0f, 0.02f, kGuidedChroma, 0.0f, 0u,
                     kAreaPrefilter | kDither | (turned ? kRotate | kSurfaceClamp : 0u) | (bars ? kBars : 0u), 0u, 0, 1, 1, 1, 0, 1};
    // q = c + R (S (A X + b) - c) + d = (R S A) X + R S b + (c - R c + d): the view's mapping as the conversion's affine map
    // (from the region's origin): its columns are where unit steps of X go.
    {
        double ox = 0, oy = 0, xx = 0, xy = 0, yx = 0, yy = 0;
        now.Forward(0, 0, ox, oy); now.Forward(1, 0, xx, xy); now.Forward(0, 1, yx, yy);
        c.view_m[0] = static_cast<float>(xx - ox); c.view_m[1] = static_cast<float>(yx - ox);
        c.view_m[2] = static_cast<float>(xy - oy); c.view_m[3] = static_cast<float>(yy - oy);
        c.view_t[0] = static_cast<float>(ox - now.left); c.view_t[1] = static_cast<float>(oy - now.top);
    }
    if (turned) ++stats_.turned;
    const auto& p = controls_;
    std::uint32_t picture = p.brightness != 0 || p.contrast != 1 || p.saturation != 1 || p.gamma != 1 || p.shadow_lift > 0 ? std::uint32_t{kGrade} : 0u;
    const std::uint64_t pixels = std::uint64_t{out_width_} * out_height_;
    if (p.micro_motion > 1 && color_.MotionPixels() >= 2 * pixels) {
        // Live pictures come in order: each is folded in. A new view size (a zoom) or a restart starts the reference over;
        // otherwise each pixel reads it where its content was in the previous picture's output.
        const bool start = micro_start_ || view.restart || !previous_valid_ || micro_size_[0] != rw || micro_size_[1] != rh ||
                           std::memcmp(previous_.a, now.a, sizeof(now.a)) != 0; // A new turn: nothing lines up.
        picture |= kMicro | kMicroFold;
        if (start) { picture |= kMicroStart; ++stats_.restarts; }
        else {
            // Output centre x -> the camera now -> where the scene was (the IMU's motion undone about the picture's centre)
            // -> the previous view's output. Affine: from three points.
            const auto& b = previous_;
            const double sc = std::cos(static_cast<double>(view.scene_roll)), ss = std::sin(static_cast<double>(view.scene_roll));
            const double mx = 0.5 * width_, my = 0.5 * height_;
            const auto map = [&](double x, double y, double& ox, double& oy) {
                double sx = 0, sy = 0; now.Forward(x, y, sx, sy);
                const double rx = sx - view.scene_dx - mx, ry = sy - view.scene_dy - my;
                b.Inverse(mx + sc * rx + ss * ry, my - ss * rx + sc * ry, ox, oy);
            };
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            map(0, 0, x0, y0); map(1, 0, x1, y1); map(0, 1, x2, y2);
            const double m[6]{x1 - x0, x2 - x0, x0, y1 - y0, y2 - y0, y0};
            const bool identity = std::fabs(m[0] - 1) < 1e-5 && std::fabs(m[1]) < 1e-5 && std::fabs(m[2]) < 1e-4 && std::fabs(m[3]) < 1e-5 &&
                                  std::fabs(m[4] - 1) < 1e-5 && std::fabs(m[5]) < 1e-4;
            if (!identity) {
                picture |= kMicroWarp; ++stats_.warped;
                for (unsigned i = 0; i < 6; ++i) c.micro_map[i] = static_cast<float>(m[i]);
            }
        }
        c.micro_read = static_cast<std::uint32_t>(micro_phase_ * pixels); c.micro_write = static_cast<std::uint32_t>((micro_phase_ ^ 1u) * pixels);
        micro_phase_ ^= 1u; micro_start_ = false; micro_size_[0] = rw; micro_size_[1] = rh;
        previous_ = now; previous_valid_ = true;
        ++stats_.micro;
    } else previous_valid_ = false;
    c.picture_flags = picture;
    c.brightness = p.brightness; c.contrast = p.contrast; c.saturation = p.saturation; c.gamma = p.gamma; c.shadow_lift = p.shadow_lift; c.micro_gain = p.micro_motion;
    const unsigned k = next_;
    if (FAILED(error_ = s.allocator->Reset()) || FAILED(error_ = s.list->Reset(s.allocator, color_.Initial()))) return Status::Failed;
    auto* list = s.list;
    if (timestamps_) list->EndQuery(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * k);
    auto b = Transition(s.source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST); list->ResourceBarrier(1, &b);
    for (unsigned plane = 0; plane < 2; ++plane) {
        D3D12_TEXTURE_COPY_LOCATION to{s.source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; to.SubresourceIndex = plane;
        D3D12_TEXTURE_COPY_LOCATION from{s.upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = planes_[plane];
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    D3D12_RESOURCE_BARRIER two[]{Transition(s.source, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                 Transition(s.image, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(2, two);
    ID3D12DescriptorHeap* heaps[]{heap_}; list->SetDescriptorHeaps(1, heaps);
    if (neural) {
        b = Transition(neural_texture_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); list->ResourceBarrier(1, &b);
        neural_.Record(list, Gpu(3 * k), Gpu(3 * kRing), left, top, right, bottom, false);
        b = Transition(neural_texture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); list->ResourceBarrier(1, &b);
        ++stats_.neural;
    }
    color_.Record(list, Gpu(3 * k), Gpu(3 * k + 2), c, neural ? Gpu(3 * kRing + 1) : Gpu(3 * k));
    two[0] = Transition(s.source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    two[1] = Transition(s.image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(2, two);
    if (neural) { b = Transition(neural_texture_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON); list->ResourceBarrier(1, &b); }
    D3D12_TEXTURE_COPY_LOCATION to{s.readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; to.PlacedFootprint = image_footprint_;
    D3D12_TEXTURE_COPY_LOCATION from{s.image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    b = Transition(s.image, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON); list->ResourceBarrier(1, &b);
    if (timestamps_) {
        list->EndQuery(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * k + 1);
        list->ResolveQueryData(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * k, 2, timestamp_readback_, 16 * k);
    }
    if (FAILED(error_ = list->Close())) return Status::Failed;
    ID3D12CommandList* lists[]{list}; queue_->ExecuteCommandLists(1, lists);
    if (FAILED(error_ = queue_->Signal(fence_, ++sequence_))) return Status::Failed;
    s.fence = sequence_; s.busy = true;
    s.info = {pts, static_cast<std::uint32_t>(sequence_), out_width_, out_height_, image_footprint_.Footprint.RowPitch,
              s.readback_data + image_footprint_.Offset, left, top, right, bottom, reconstruction, picture, -1, turned ? roll : 0.0f, ten_bit_,
              {shown[0], shown[1], shown[2], shown[3]}};
    s.info.fitted = bars; s.info.picture_left = inside[0]; s.info.picture_top = inside[1]; s.info.picture_right = inside[2]; s.info.picture_bottom = inside[3];
    ++stats_.ingested; next_ = (next_ + 1) % kRing;
    return Status::Ready;
}
bool LiveGpuPipeline::Latest(LiveImage& image) noexcept {
    Collect();
    if (latest_ < 0 || slots_[latest_].busy) return false;
    image = slots_[latest_].info;
    return true;
}
bool LiveGpuPipeline::Drain(unsigned milliseconds) noexcept {
    if (!fence_ || !wait_) return false;
    if (fence_->GetCompletedValue() < sequence_) {
        if (FAILED(fence_->SetEventOnCompletion(sequence_, wait_)) || WaitForSingleObject(wait_, milliseconds) != WAIT_OBJECT_0) return false;
    }
    Collect();
    return fence_->GetCompletedValue() != UINT64_MAX;
}
}
