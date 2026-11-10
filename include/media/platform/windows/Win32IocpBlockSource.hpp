#pragma once

#include "media/core/linear_memory_arena.hpp"
#include "media/platform/IBlockSource.hpp"
#include <Windows.h>

namespace media::platform::windows {
struct FileSourceOptions {
    std::uint32_t block_bytes = 1024 * 1024;
    std::uint32_t block_count = 16;
    bool unbuffered = true;
    std::uint32_t lease_count = 1024; // Ranges that may be checked out at once (several share one block).
};

// Overlapped file reads into a fixed set of blocks that double as a read cache.
// - A request whose range lies inside a block already read, or being read, takes a lease on that block: no I/O.
//   Samples of a media file lie next to each other, so one block read serves every sample inside it; a demuxer that
//   asks for one sample at a time costs one read per block, not one per sample.
// - Otherwise the least recently used block that nobody holds is read again from the request's offset, a whole
//   block at once (read-ahead). Blocks nobody holds keep their bytes until then, so a seek back into them is free.
// - A lease pins its block: the view stays valid and unchanged until Release, whatever is requested meanwhile.
// No allocation after Open, no lock, one owner thread (IBlockSource's contract).
class Win32IocpBlockSource final : public IBlockSource {
public:
    Win32IocpBlockSource() noexcept = default;
    ~Win32IocpBlockSource() noexcept override;
    Win32IocpBlockSource(const Win32IocpBlockSource&) = delete;
    Win32IocpBlockSource& operator=(const Win32IocpBlockSource&) = delete;

    [[nodiscard]] IoStatus Open(const wchar_t* path, core::LinearMemoryArena& arena,
                                 FileSourceOptions options = {}) noexcept;
    [[nodiscard]] std::uint64_t Size() const noexcept override { return size_; }
    [[nodiscard]] std::uint32_t MaxRangeBytes() const noexcept override {
        return block_bytes_ >= alignment_ ? block_bytes_ - (alignment_ - 1) : 0;
    }
    // WouldBlock: every lease is out, or the range is in no block and every block is held or being read.
    [[nodiscard]] IoStatus Request(std::uint64_t offset, std::uint32_t bytes,
                                   BlockToken& token) noexcept override;
    [[nodiscard]] IoStatus Poll(BlockToken token, BlockView& view) noexcept override;
    [[nodiscard]] bool Release(BlockToken token) noexcept override;
    // At most budget zero-timeout dequeue calls. Returns completion count.
    std::uint32_t Pump(std::uint32_t budget = 16) noexcept;
    // Preparing/quiescent ownership transfer only. IOCP associates polling threads
    // even after a zero-timeout call; detach the old owner before it runs other work.
    [[nodiscard]] bool DetachPollingThread() noexcept;
    void BeginCancel() noexcept;
    // Nonblocking; returns WouldBlock while reads or leases remain.
    [[nodiscard]] IoStatus Close() noexcept;
    [[nodiscard]] std::uint32_t Alignment() const noexcept { return alignment_; }
    [[nodiscard]] DWORD LastOsError() const noexcept { return error_; }
    [[nodiscard]] std::uint32_t Outstanding() const noexcept { return pending_; } // Reads in flight.
    [[nodiscard]] std::uint32_t Leases() const noexcept { return leased_; }       // Ranges checked out.
    struct Statistics { std::uint64_t requests = 0, reads = 0, read_bytes = 0; }; // Requests served; of those, by a new read.
    [[nodiscard]] const Statistics& Stats() const noexcept { return stats_; }

private:
    enum class State : std::uint8_t { Empty, Pending, Ready, Failed };
    struct Block {
        OVERLAPPED overlapped{}; // First member: a completion's OVERLAPPED* is its block.
        std::byte* data = nullptr;
        std::uint64_t offset = 0, used = 0; // File offset of data[0]; tick of the latest lease (least recent is reused).
        std::uint32_t bytes = 0, planned = 0, leases = 0; // Bytes read; bytes asked of the OS, bounded by the file end.
        DWORD error = 0;
        State state = State::Empty;
    };
    struct Lease { std::uint32_t generation = 0, block = 0; bool active = false; };
    [[nodiscard]] Lease* Find(BlockToken token) noexcept;
    bool Dequeue(DWORD timeout) noexcept;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE port_ = nullptr;
    Block* blocks_ = nullptr;
    Lease* leases_ = nullptr;
    std::uint64_t size_ = 0, tick_ = 0;
    std::uint32_t count_ = 0, lease_count_ = 0;
    std::uint32_t block_bytes_ = 0;
    std::uint32_t alignment_ = 1;
    std::uint32_t pending_ = 0, leased_ = 0;
    std::uint32_t next_lease_ = 0;
    std::uint32_t next_generation_ = 0; // Survives Close/Open; stale tokens never revive.
    DWORD error_ = 0;
    bool cancelling_ = false;
    Statistics stats_{};
};
} // namespace media::platform::windows
