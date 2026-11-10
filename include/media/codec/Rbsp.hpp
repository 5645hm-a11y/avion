#pragma once
#include "media/core/linear_memory_arena.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace media::codec {
// Reads EBSP directly, removing emulation-prevention bytes logically, never copying RBSP.
class RbspReader final {
public:
    explicit RbspReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}
    std::uint32_t Bits(unsigned count) noexcept;
    std::uint32_t Ue() noexcept;
    std::int32_t Se() noexcept;
    void Skip(unsigned count) noexcept { while (count && good_) { const auto n = count > 32 ? 32u : count; (void)Bits(n); count -= n; } }
    bool Good() const noexcept { return good_; }
    bool MoreData() const noexcept;
    bool Finish() noexcept;
    // RBSP bits consumed so far; emulation-prevention bytes are not counted.
    std::size_t Position() const noexcept { return position_; }
private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0, position_ = 0;
    unsigned remaining_ = 0, current_ = 0, zeros_ = 0;
    bool good_ = true;
};
// Discard: a decodable-order picture that must not be decoded (RASL after a random-access start).
enum class SyntaxStatus : std::uint8_t { Ready, Invalid, Unsupported, NeedIdr, Busy, Discard };

// Setup-time helper; value-initializes a fixed table inside the arena.
template <class T> T* ArenaArray(core::LinearMemoryArena& arena, unsigned count) noexcept {
    auto* p = static_cast<T*>(arena.Allocate(sizeof(T) * count, alignof(T)));
    if (p) for (unsigned i = 0; i < count; ++i) std::construct_at(p + i);
    return p;
}
}
