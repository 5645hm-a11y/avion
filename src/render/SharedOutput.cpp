// Texture output's shared textures and fences (include/media/render/SharedOutput.hpp; docs/sdk.md, "Texture output").
#include "media/render/SharedOutput.hpp"
#include <initializer_list>

namespace media::render {
namespace {
template <typename T> void Drop(T*& object) noexcept { if (object) { object->Release(); object = nullptr; } }
}
bool SharedOutput::CreateFences(ID3D12Device* device) noexcept {
    if (!device || fence_) return false;
    HANDLE handle = nullptr;
    error_ = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_));
    if (SUCCEEDED(error_)) error_ = device->CreateSharedHandle(fence_, nullptr, GENERIC_ALL, nullptr, &handle);
    if (SUCCEEDED(error_)) { fence_handle_.store(handle, std::memory_order_release); handle = nullptr; }
    if (SUCCEEDED(error_)) error_ = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&host_fence_));
    if (SUCCEEDED(error_)) error_ = device->CreateSharedHandle(host_fence_, nullptr, GENERIC_ALL, nullptr, &handle);
    if (SUCCEEDED(error_)) host_fence_handle_.store(handle, std::memory_order_release);
    adapter_ = device->GetAdapterLuid();
    return SUCCEEDED(error_);
}
bool SharedOutput::CreateTextures(ID3D12Device* device, unsigned width, unsigned height, Format format, D3D12_RESOURCE_FLAGS flags) noexcept {
    // Committed textures on a shared heap, in the common state between frames (D3D12_RESOURCE_STATE_PRESENT is COMMON), which
    // is the state another API's device reads them in; their NT handles; a new generation for the host.
    if (!device || !width || !height || width > 8192 || height > 8192) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = width; d.Height = height; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.Format = Dxgi(format); d.SampleDesc.Count = 1; d.Flags = flags;
    format_ = format;
    for (unsigned i = 0; i != kCount; ++i) {
        error_ = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&textures_[i]));
        HANDLE handle = nullptr;
        if (SUCCEEDED(error_)) error_ = device->CreateSharedHandle(textures_[i], nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(error_)) return false;
        slots_[i].handle.store(handle, std::memory_order_release);
        slots_[i].state.store(0, std::memory_order_relaxed); slots_[i].host_wait.store(0, std::memory_order_relaxed);
    }
    latest_.store(0, std::memory_order_release);
    size_[0].store(width, std::memory_order_relaxed); size_[1].store(height, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_release);
    return true;
}
void SharedOutput::ReleaseTextures() noexcept {
    generation_.fetch_add(1, std::memory_order_acq_rel); // A host reading the handles now sees them change.
    latest_.store(0, std::memory_order_release);
    for (unsigned i = 0; i != kCount; ++i) {
        if (void* handle = slots_[i].handle.exchange(nullptr)) CloseHandle(handle);
        Drop(textures_[i]);
    }
}
void SharedOutput::Release() noexcept {
    ReleaseTextures();
    Drop(fence_); Drop(host_fence_);
    for (auto* h : {&fence_handle_, &host_fence_handle_}) if (void* handle = h->exchange(nullptr)) CloseHandle(handle);
    value_ = 0; sequence_ = 0; next_ = 0;
}
bool SharedOutput::Publish(ID3D12CommandQueue* queue, unsigned i, std::int64_t pts, std::uint32_t transfer) noexcept {
    const auto value = ++value_;
    if (FAILED(error_ = queue->Signal(fence_, value))) { slots_[i].state.store(0, std::memory_order_release); return false; }
    auto& s = slots_[i];
    s.pts = pts; s.value = value; s.sequence = ++sequence_; s.transfer = transfer;
    s.state.store(1, std::memory_order_release);
    // The newest now; the one before it, if the host never took it, is free again (a host that took it in between keeps it).
    if (const auto before = latest_.exchange(value << 2 | i, std::memory_order_acq_rel)) {
        std::uint32_t published = 1;
        (void)slots_[before & 3].state.compare_exchange_strong(published, 0, std::memory_order_acq_rel);
    }
    return true;
}
bool SharedOutput::Describe(Desc& out) const noexcept {
    const auto generation = generation_.load(std::memory_order_acquire);
    for (unsigned i = 0; i != kCount; ++i) out.textures[i] = slots_[i].handle.load(std::memory_order_acquire);
    out.fence = fence_handle_.load(std::memory_order_acquire); out.host_fence = host_fence_handle_.load(std::memory_order_acquire);
    out.width = size_[0].load(std::memory_order_relaxed); out.height = size_[1].load(std::memory_order_relaxed);
    out.generation = generation; out.adapter = adapter_; out.format = Dxgi(format_);
    // A resize in between: asked again (the handles of one generation only).
    return out.textures[0] && out.fence && generation_.load(std::memory_order_acquire) == generation;
}
bool SharedOutput::Acquire(Frame& out) noexcept {
    for (unsigned attempt = 0; attempt != 4; ++attempt) {
        const auto latest = latest_.load(std::memory_order_acquire);
        if (!latest) return false;
        auto& s = slots_[latest & 3];
        std::uint32_t published = 1;
        if (s.state.compare_exchange_strong(published, 2, std::memory_order_acq_rel)) {
            // Its own fields, not latest's: the producer may have freed, redrawn and republished it since the load above.
            out.index = static_cast<unsigned>(latest & 3); out.fence_value = s.value; out.pts = s.pts; out.sequence = s.sequence; out.transfer = s.transfer;
            out.generation = generation_.load(std::memory_order_acquire);
            return true;
        }
        if (published == 2) return false; // The host holds the newest already: nothing newer.
    }
    return false;
}
bool SharedOutput::ReleaseFrame(unsigned index, std::uint64_t host_value) noexcept {
    if (index >= kCount) return false;
    slots_[index].host_wait.store(host_value, std::memory_order_relaxed);
    std::uint32_t held = 2;
    return slots_[index].state.compare_exchange_strong(held, 0, std::memory_order_acq_rel);
}
}
