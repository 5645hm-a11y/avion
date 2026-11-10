#pragma once
#include <d3d12.h>
#include <atomic>
#include <cstdint>

namespace media::render {
// Texture output (docs/sdk.md, "Texture output: API 13", "API 14"): three textures on a shared heap with NT handles, a shared
// frames fence (signalled by the engine's queue once a frame is drawn) and a shared host fence (signalled by the host once its
// reads of a texture are queued), for a host renderer on another device (Direct3D 11 or 12, another process). Used by the
// windowed engine's renderer (D3D12Renderer, composites drawn into them) and by host mode's GPU decoding (the SDK's HostGpu,
// pictures converted into them).
//
// Each texture's state is one atomic: 0 free, 1 published (the newest), 2 held by the host, 3 being drawn; the newest frame is
// one word, (fence value << 2) | index. The producer claims a free texture whose previous work has retired (the caller's own
// predicate), draws, makes its queue wait for the host's release value first (HostWait), and publishes after signalling: a newer
// publication frees a published texture the host never took. The host acquires the newest (published -> held, its fields read
// from the texture's own slot, never from the word: see Acquire) and releases it with its host fence's value. One producer
// thread, one host thread; nothing locks, nothing allocates after Create.
//
// Formats (API 14): 8-bit R8G8B8A8_UNORM (display-referred, BT.709 gamma: SDR, HDR tone-mapped), 10-bit R10G10B10A2_UNORM (PQ,
// BT.2020: an HDR10 signal; SDR pictures at reference white, 203 cd/m2) or R16G16B16A16_FLOAT (scRGB: linear BT.709, 1.0 =
// 80 cd/m2; SDR white 203 / 80), for a host compositor that shows HDR.
class SharedOutput final {
public:
    static constexpr unsigned kCount = 3;
    enum class Format : std::uint8_t { Rgba8 = 0, Pq10 = 1, Scrgb16 = 2 };
    [[nodiscard]] static DXGI_FORMAT Dxgi(Format f) noexcept {
        return f == Format::Pq10 ? DXGI_FORMAT_R10G10B10A2_UNORM : f == Format::Scrgb16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    // DXGI_COLOR_SPACE_TYPE: RGB_FULL_G22_NONE_P709 (0), RGB_FULL_G2084_NONE_P2020 (12), RGB_FULL_G10_NONE_P709 (1).
    [[nodiscard]] static std::uint32_t ColorSpace(Format f) noexcept { return f == Format::Pq10 ? 12u : f == Format::Scrgb16 ? 1u : 0u; }
    static constexpr float kSdrWhiteNits = 203.0f; // BT.2408 reference white: where an SDR picture's white lands in an HDR format.
    struct Desc { void* textures[kCount]; void* fence; void* host_fence; unsigned width, height, generation; DXGI_FORMAT format; LUID adapter; };
    struct Frame { unsigned index, generation, sequence, transfer; std::uint64_t fence_value; std::int64_t pts; };

    SharedOutput() noexcept = default;
    SharedOutput(const SharedOutput&) = delete;
    SharedOutput& operator=(const SharedOutput&) = delete;
    ~SharedOutput() noexcept { Release(); }
    // The two shared fences and their handles (setup only).
    [[nodiscard]] bool CreateFences(ID3D12Device* device) noexcept;
    // Three textures of this size and format (a new generation); `flags`: how the producer draws (render target, UAV).
    [[nodiscard]] bool CreateTextures(ID3D12Device* device, unsigned width, unsigned height, Format format, D3D12_RESOURCE_FLAGS flags) noexcept;
    void ReleaseTextures() noexcept; // The handles closed, the generation moved on (a host reading them sees the change).
    void Release() noexcept;         // Everything (the device idle for these textures).
    [[nodiscard]] ID3D12Resource* Texture(unsigned i) const noexcept { return i < kCount ? textures_[i] : nullptr; }
    [[nodiscard]] ID3D12Fence* HostFence() const noexcept { return host_fence_; }
    [[nodiscard]] Format TextureFormat() const noexcept { return format_; }
    [[nodiscard]] unsigned Width() const noexcept { return size_[0].load(std::memory_order_relaxed); }
    [[nodiscard]] unsigned Height() const noexcept { return size_[1].load(std::memory_order_relaxed); }
    // The producer: a free texture whose previous drawing `retired(i)` says is done, now being drawn (-1: none free, counted).
    template <class Retired> int Claim(Retired retired) noexcept {
        for (unsigned k = 0; k != kCount; ++k) {
            const unsigned i = (next_ + k) % kCount;
            std::uint32_t free = 0;
            if (retired(i) && slots_[i].state.compare_exchange_strong(free, 3, std::memory_order_acq_rel)) { next_ = i + 1; return static_cast<int>(i); }
        }
        dropped_.fetch_add(1, std::memory_order_relaxed); // Every texture published or held: the host holds two.
        return -1;
    }
    void Unclaim(unsigned i) noexcept { slots_[i].state.store(0, std::memory_order_release); } // Claimed, then not drawn.
    // The host's release value to wait for (on the producer's queue) before drawing into texture `i` again (0: none).
    [[nodiscard]] std::uint64_t HostWait(unsigned i) const noexcept { return slots_[i].host_wait.load(std::memory_order_relaxed); }
    // After the drawing was submitted on `queue`: the frames fence signalled with the next value, then the texture published.
    // `transfer`: the picture's H.273 transfer characteristics (1 SDR, 16 PQ, 18 HLG; 0 unknown). False: the signal failed (freed).
    bool Publish(ID3D12CommandQueue* queue, unsigned i, std::int64_t pts, std::uint32_t transfer) noexcept;
    // The host (any one thread).
    [[nodiscard]] bool Describe(Desc& out) const noexcept;   // False: a generation change under way (ask again).
    [[nodiscard]] bool Acquire(Frame& out) noexcept;         // False: none yet, or none newer than the one held.
    [[nodiscard]] bool ReleaseFrame(unsigned index, std::uint64_t host_value) noexcept;
    [[nodiscard]] std::uint32_t Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t Published() const noexcept { return sequence_; }
    [[nodiscard]] HRESULT LastError() const noexcept { return error_; }
private:
    struct Slot {
        std::atomic<std::uint32_t> state{0};
        std::atomic<std::uint64_t> host_wait{0};     // The host fence's value to wait for before drawing into it again.
        std::atomic<void*> handle{nullptr};          // Its NT handle (CreateSharedHandle).
        std::int64_t pts = 0;                        // The media time of the picture drawn into it.
        std::uint64_t value = 0;                     // The frames fence's value its drawing signals (written before publishing).
        std::uint32_t sequence = 0, transfer = 0;
    };
    Slot slots_[kCount];
    ID3D12Resource* textures_[kCount]{};
    ID3D12Fence* fence_ = nullptr;                   // Signalled by the producer's queue: a frame drawn.
    ID3D12Fence* host_fence_ = nullptr;              // Signalled by the host: it is done with a texture.
    std::atomic<void*> fence_handle_{nullptr}, host_fence_handle_{nullptr};
    std::uint64_t value_ = 0;
    std::uint32_t sequence_ = 0;
    unsigned next_ = 0;
    Format format_ = Format::Rgba8;
    std::atomic<std::uint64_t> latest_{0};           // (fence value << 2) | index of the newest published frame; 0: none.
    std::atomic<std::uint32_t> generation_{0}, dropped_{0};
    std::atomic<std::uint32_t> size_[2]{};
    LUID adapter_{};
    HRESULT error_ = S_OK;
};
}
