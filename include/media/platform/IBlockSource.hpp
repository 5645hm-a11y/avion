#pragma once

#include "media/core/platform.hpp"

namespace media::platform {
enum class IoStatus : std::uint8_t {
    Ready, Pending, WouldBlock, InvalidArgument, InvalidToken, OutOfRange,
    TooLarge, Cancelled, OsError, Closed
};
struct BlockToken {
    std::uint32_t id = UINT32_MAX;
    std::uint32_t generation = 0;
};
struct BlockView {
    const std::byte* data = nullptr;
    std::uint64_t file_offset = 0;
    std::uint32_t valid_bytes = 0;
    BlockToken token{};
};

// Single owner calls this interface. Consumers return tokens through their own
// SPSC return queue, never by calling Release concurrently with the owner.
// A provider exposes an immutable, known-length object with range access.
class IBlockSource {
public:
    virtual ~IBlockSource() = default;
    [[nodiscard]] virtual std::uint64_t Size() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t MaxRangeBytes() const noexcept = 0;
    // Accepted requests return Pending and a unique lease. Poll may complete in
    // any order. Ready views stay immutable until Release, including aliases.
    [[nodiscard]] virtual IoStatus Request(std::uint64_t offset, std::uint32_t bytes,
                                           BlockToken& token) noexcept = 0;
    [[nodiscard]] virtual IoStatus Poll(BlockToken token, BlockView& view) noexcept = 0;
    // Pending release abandons interest, but storage is retained until completion.
    [[nodiscard]] virtual bool Release(BlockToken token) noexcept = 0;
};
} // namespace media::platform
