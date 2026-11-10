#pragma once
#include "media/core/linear_memory_arena.hpp"
#include "media/platform/IGpuContext.hpp"

namespace media::video {
// Decode-owner state only. Consumer returns arrive through a separate SPSC edge.
// One joined consumer timeline covers every downstream GPU reader.
class SurfaceLeasePool final {
public:
    [[nodiscard]] bool Initialize(core::LinearMemoryArena& arena, std::uint32_t count) noexcept {
        if (slots_ || !count || count > 127) return false;
        slots_ = static_cast<Slot*>(arena.Allocate(sizeof(Slot) * count, alignof(Slot)));
        if (!slots_) return false;
        for (std::uint32_t i = 0; i < count; ++i) std::construct_at(slots_ + i);
        count_ = count; return true;
    }
    [[nodiscard]] bool Reserve(std::uint64_t decoded, std::uint64_t consumed, platform::SurfaceHandle& out) noexcept {
        if (decoded == UINT64_MAX || consumed == UINT64_MAX) return false;
        for (std::uint32_t i = 0; i < count_; ++i) {
            auto& slot = slots_[i];
            if (slot.active && (slot.reference || !slot.returned || !slot.submitted ||
                slot.decode_value > decoded || slot.consumer_value > consumed)) continue;
            if (slot.generation == UINT32_MAX) continue;
            const auto generation = slot.generation + 1;
            slot = {}; slot.active = true; slot.generation = generation;
            out = {i, generation}; return true;
        }
        return false;
    }
    // Whether Reserve would find a slot now (no side effect): host mode's GPU decoding asks before it parses a picture.
    [[nodiscard]] bool CanReserve(std::uint64_t decoded, std::uint64_t consumed) const noexcept {
        if (decoded == UINT64_MAX || consumed == UINT64_MAX) return false;
        for (std::uint32_t i = 0; i < count_; ++i) {
            const auto& slot = slots_[i];
            if (slot.active && (slot.reference || !slot.returned || !slot.submitted || slot.decode_value > decoded || slot.consumer_value > consumed)) continue;
            if (slot.generation != UINT32_MAX) return true;
        }
        return false;
    }
    [[nodiscard]] bool Cancel(platform::SurfaceHandle handle) noexcept {
        auto* slot = Find(handle);
        if (!slot || slot->submitted) return false;
        slot->active = false; return true;
    }
    // `unread`: no consumer will read the surface (a reference-only picture, retired with consumer fence 0). As a
    // reference it is usable at once: only the decode queue touches it, in submission order, so the next picture
    // need not wait for its consumer return (which still frees the slot for reuse).
    [[nodiscard]] bool Submitted(platform::SurfaceHandle handle, std::uint64_t value, bool reference, bool unread = false) noexcept {
        auto* slot = Find(handle);
        if (!slot || slot->submitted || !value || value == UINT64_MAX) return false;
        slot->submitted = true; slot->decode_value = value; slot->reference = reference; slot->unread = unread; return true;
    }
    [[nodiscard]] bool Return(platform::SurfaceHandle handle, std::uint64_t value) noexcept {
        auto* slot = Find(handle);
        if (!slot || !slot->submitted || slot->returned || value == UINT64_MAX) return false;
        slot->returned = true; slot->consumer_value = value; return true;
    }
    [[nodiscard]] bool ReferenceReady(platform::SurfaceHandle handle, std::uint64_t& consumer_value) const noexcept {
        const auto* slot = Find(handle);
        if (!slot || !slot->submitted || !slot->reference || (!slot->returned && !slot->unread)) return false;
        consumer_value = slot->returned ? slot->consumer_value : 0; return true;
    }
    [[nodiscard]] bool ReferenceUsed(platform::SurfaceHandle handle, std::uint64_t decode_value) noexcept {
        auto* slot = Find(handle);
        if (!slot || !slot->reference || decode_value < slot->decode_value || decode_value == UINT64_MAX) return false;
        slot->decode_value = decode_value; return true;
    }
    [[nodiscard]] bool ReleaseReference(platform::SurfaceHandle handle) noexcept {
        auto* slot = Find(handle);
        if (!slot || !slot->reference) return false;
        slot->reference = false; return true;
    }
    [[nodiscard]] bool Valid(platform::SurfaceHandle handle) const noexcept { return Find(handle) != nullptr; }
    // Device loss: every lease of the lost device is void (its surfaces no longer exist). Slots become free with
    // their generations kept, so a stale handle can never validate against the rebuilt pool.
    void Reset() noexcept {
        for (std::uint32_t i = 0; i < count_; ++i) { const auto generation = slots_[i].generation; slots_[i] = {}; slots_[i].generation = generation; }
    }
    void Forget() noexcept { slots_ = nullptr; count_ = 0; } // Its arena is about to be reused: Initialize again before use.
    [[nodiscard]] bool AllConsumersReturned() const noexcept {
        for (std::uint32_t i = 0; i < count_; ++i)
            if (slots_[i].active && slots_[i].submitted && !slots_[i].returned) return false;
        return true;
    }
private:
    struct Slot {
        std::uint64_t decode_value = 0, consumer_value = 0;
        std::uint32_t generation = 0;
        bool active = false, submitted = false, reference = false, returned = false, unread = false;
    };
    Slot* Find(platform::SurfaceHandle handle) const noexcept {
        if (handle.id >= count_ || !slots_[handle.id].active || slots_[handle.id].generation != handle.generation) return nullptr;
        return slots_ + handle.id;
    }
    Slot* slots_ = nullptr;
    std::uint32_t count_ = 0;
};
}
