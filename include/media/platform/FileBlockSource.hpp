#pragma once
#include "media/core/linear_memory_arena.hpp"
#include "media/platform/IBlockSource.hpp"
#include <cstdio>

namespace media::platform {
// A local file as an IBlockSource, on every platform (docs/sdk.md, "Host-driven playback"): kSlots blocks of kBlockBytes
// from the arena; a request reads its range at once (unbuffered stdio, so no library buffer is allocated after open) and
// polls Ready. The reads happen on the caller's thread: the host-driven engine's step. Open and Close are setup; the rest
// allocates nothing.
class FileBlockSource final : public IBlockSource {
public:
    static constexpr unsigned kSlots = 8;
    static constexpr std::uint32_t kBlockBytes = 1u << 20;
    // API 14: host mode's GPU decoding opens with 4 MiB blocks (a 4K picture's sample can be larger than 1 MiB).
    static constexpr std::uint32_t kLargeBlockBytes = 4u << 20;
    FileBlockSource() noexcept = default;
    FileBlockSource(const FileBlockSource&) = delete;
    FileBlockSource& operator=(const FileBlockSource&) = delete;
    ~FileBlockSource() noexcept override { Close(); }
    // `utf8_path`: a local path (UTF-8 on every platform); kSlots blocks of `block_bytes` (kBlockBytes or kLargeBlockBytes). False
    // when it cannot be opened or the arena is exhausted.
    [[nodiscard]] bool Open(core::LinearMemoryArena& arena, const char* utf8_path, std::uint32_t block_bytes = kBlockBytes) noexcept;
    void Close() noexcept;
    [[nodiscard]] std::uint64_t Size() const noexcept override { return size_; }
    [[nodiscard]] std::uint32_t MaxRangeBytes() const noexcept override { return block_bytes_; }
    [[nodiscard]] IoStatus Request(std::uint64_t offset, std::uint32_t bytes, BlockToken& token) noexcept override;
    [[nodiscard]] IoStatus Poll(BlockToken token, BlockView& view) noexcept override;
    [[nodiscard]] bool Release(BlockToken token) noexcept override;
    [[nodiscard]] std::uint64_t BytesRead() const noexcept { return read_; }
    // Reads [offset, offset + bytes) into `out` directly (setup: codec records, sidecar text).
    [[nodiscard]] bool ReadAt(std::uint64_t offset, void* out, std::uint32_t bytes) noexcept;
private:
    struct Slot { std::byte* data = nullptr; BlockView view{}; std::uint32_t generation = 0; bool active = false; };
    std::FILE* file_ = nullptr;
    std::uint64_t size_ = 0, read_ = 0;
    std::uint32_t block_bytes_ = kBlockBytes;
    Slot slots_[kSlots]{};
};
// Opens a UTF-8 path for reading in binary (wide on Windows); nullptr when it cannot.
[[nodiscard]] std::FILE* OpenUtf8(const char* utf8_path) noexcept;
// Reads a whole file (UTF-8 path) into `out` through the OS (CreateFileW/ReadFile, open/read): no stdio stream, nothing allocated
// (API 17, subtitles after open). False: it cannot be opened or read, or it is larger than `capacity`.
[[nodiscard]] bool ReadWholeFile(const char* utf8_path, void* out, std::size_t capacity, std::size_t& bytes) noexcept;
}
