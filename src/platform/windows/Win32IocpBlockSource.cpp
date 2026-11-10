#include "media/platform/windows/Win32IocpBlockSource.hpp"

#include <algorithm>
#include <memory>

namespace media::platform::windows {
namespace {
bool PowerOfTwo(const std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}
}

Win32IocpBlockSource::~Win32IocpBlockSource() noexcept {
    // Teardown only: cancellation does not end the kernel's use of OVERLAPPED
    // or payload storage. Arena must outlive this object and all consumer views.
    BeginCancel();
    while (pending_ != 0) (void)Dequeue(INFINITE);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    if (port_ != nullptr) CloseHandle(port_);
}

IoStatus Win32IocpBlockSource::Open(const wchar_t* path, core::LinearMemoryArena& arena,
                                const FileSourceOptions options) noexcept {
    if (file_ != INVALID_HANDLE_VALUE || path == nullptr || options.block_count == 0 ||
        options.block_count > 65536 || options.block_bytes == 0 || options.lease_count == 0 ||
        options.lease_count > 1u << 20 || arena.IsSealed())
        return IoStatus::InvalidArgument;
    error_ = 0;
    const DWORD flags = FILE_FLAG_OVERLAPPED | (options.unbuffered ? FILE_FLAG_NO_BUFFERING : 0);
    // Deny writers/deletion for the lifetime of this immutable byte object.
    file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) { error_ = GetLastError(); return IoStatus::OsError; }
    LARGE_INTEGER length{};
    FILE_STORAGE_INFO storage{};
    alignment_ = 1;
    if (!GetFileSizeEx(file_, &length) || length.QuadPart < 0) {
        error_ = GetLastError(); (void)Close(); return IoStatus::OsError;
    }
    if (options.unbuffered) {
        if (!GetFileInformationByHandleEx(file_, FileStorageInfo, &storage, sizeof(storage))) {
            error_ = GetLastError(); (void)Close(); return IoStatus::OsError;
        }
        alignment_ = (std::max)({storage.LogicalBytesPerSector,
            storage.PhysicalBytesPerSectorForAtomicity, storage.PhysicalBytesPerSectorForPerformance,
            storage.FileSystemEffectivePhysicalBytesPerSectorForAtomicity});
        if (!PowerOfTwo(storage.LogicalBytesPerSector) || !PowerOfTwo(alignment_) ||
            options.block_bytes % alignment_ != 0) {
            (void)Close(); return IoStatus::InvalidArgument;
        }
    }
    port_ = CreateIoCompletionPort(file_, nullptr, 1, 1);
    if (port_ == nullptr) { error_ = GetLastError(); (void)Close(); return IoStatus::OsError; }
    auto* memory = arena.Allocate(sizeof(Block) * options.block_count, alignof(Block));
    auto* lease_memory = arena.Allocate(sizeof(Lease) * options.lease_count, alignof(Lease));
    if (memory == nullptr || lease_memory == nullptr) { (void)Close(); return IoStatus::TooLarge; }
    blocks_ = static_cast<Block*>(memory); leases_ = static_cast<Lease*>(lease_memory);
    count_ = options.block_count; lease_count_ = options.lease_count;
    for (std::uint32_t i = 0; i < count_; ++i) std::construct_at(blocks_ + i);
    for (std::uint32_t i = 0; i < lease_count_; ++i) std::construct_at(leases_ + i);
    for (std::uint32_t i = 0; i < count_; ++i) {
        blocks_[i].data = static_cast<std::byte*>(arena.Allocate(options.block_bytes, alignment_));
        if (blocks_[i].data == nullptr) { (void)Close(); return IoStatus::TooLarge; }
    }
    block_bytes_ = options.block_bytes;
    size_ = static_cast<std::uint64_t>(length.QuadPart);
    next_lease_ = 0; leased_ = 0; tick_ = 0; stats_ = {};
    cancelling_ = false;
    return IoStatus::Ready;
}

Win32IocpBlockSource::Lease* Win32IocpBlockSource::Find(const BlockToken token) noexcept {
    if (token.id >= lease_count_ || leases_ == nullptr) return nullptr;
    auto& lease = leases_[token.id];
    return lease.active && lease.generation == token.generation ? &lease : nullptr;
}

IoStatus Win32IocpBlockSource::Request(const std::uint64_t offset, const std::uint32_t bytes,
                                   BlockToken& token) noexcept {
    token = {};
    if (file_ == INVALID_HANDLE_VALUE) return IoStatus::Closed;
    if (cancelling_) return IoStatus::Cancelled;
    if (bytes == 0) return IoStatus::InvalidArgument;
    if (offset > size_ || bytes > size_ - offset) return IoStatus::OutOfRange;
    const auto base = offset & ~static_cast<std::uint64_t>(alignment_ - 1);
    const auto needed = offset - base + bytes;
    if (needed > block_bytes_) return IoStatus::TooLarge;
    if (leased_ == lease_count_) return IoStatus::WouldBlock;
    if (next_generation_ == UINT32_MAX) return IoStatus::TooLarge;
    // A block that holds the range, or will once its read completes; else the least recently used free block.
    std::uint32_t chosen = count_, victim = count_;
    for (std::uint32_t i = 0; i < count_; ++i) {
        const auto& block = blocks_[i];
        const auto extent = block.state == State::Ready ? block.bytes : block.state == State::Pending ? block.planned : 0u;
        if (extent && offset >= block.offset && offset - block.offset <= extent && bytes <= extent - (offset - block.offset)) { chosen = i; break; }
        if (block.leases == 0 && block.state != State::Pending && (victim == count_ || block.used < blocks_[victim].used)) victim = i;
    }
    if (chosen == count_) {
        if (victim == count_) return IoStatus::WouldBlock;
        auto& block = blocks_[victim];
        block.overlapped = {};
        block.overlapped.Offset = static_cast<DWORD>(base);
        block.overlapped.OffsetHigh = static_cast<DWORD>(base >> 32);
        block.offset = base; block.bytes = 0; block.error = 0;
        // Read ahead within this fixed block. Short final reads are permitted;
        // requested OS size and offset remain sector multiples.
        const auto available = (std::min)(static_cast<std::uint64_t>(block_bytes_), size_ - base);
        const auto read_bytes = static_cast<DWORD>((available + alignment_ - 1) &
                                                  ~static_cast<std::uint64_t>(alignment_ - 1));
        block.planned = static_cast<std::uint32_t>(available);
        block.state = State::Pending;
        ++pending_;
        const BOOL result = ReadFile(file_, block.data, read_bytes, nullptr, &block.overlapped);
        if (!result) {
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                --pending_;
                block.state = State::Empty;
                error_ = error;
                return IoStatus::OsError;
            }
        }
        // Synchronous success STILL posts an IOCP packet: never retire it here.
        ++stats_.reads; stats_.read_bytes += read_bytes;
        chosen = victim;
    }
    while (leases_[next_lease_].active) next_lease_ = (next_lease_ + 1) % lease_count_; // One is free: leased_ < lease_count_.
    const auto id = next_lease_;
    next_lease_ = (next_lease_ + 1) % lease_count_;
    leases_[id] = {++next_generation_, chosen, true};
    ++leased_; ++blocks_[chosen].leases; blocks_[chosen].used = ++tick_; ++stats_.requests;
    token = {id, leases_[id].generation};
    return IoStatus::Pending;
}

bool Win32IocpBlockSource::Dequeue(const DWORD timeout) noexcept {
    if (port_ == nullptr) return false;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* overlapped = nullptr;
    const BOOL ok = GetQueuedCompletionStatus(port_, &bytes, &key, &overlapped, timeout);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (overlapped == nullptr) {
        if (error != WAIT_TIMEOUT) error_ = error;
        return false;
    }
    // The dedicated port only receives this source's stable blocks.
    auto* block = reinterpret_cast<Block*>(overlapped);
    --pending_;
    // Never more than the file holds past the block's offset (an unbuffered read is rounded up to whole sectors).
    block->bytes = static_cast<std::uint32_t>((std::min)(std::uint64_t{bytes}, size_ - block->offset));
    block->error = error;
    // A failed read that nobody waits for leaves nothing behind.
    block->state = ok ? State::Ready : block->leases ? State::Failed : State::Empty;
    if (!ok) error_ = error;
    return true;
}

std::uint32_t Win32IocpBlockSource::Pump(const std::uint32_t budget) noexcept {
    std::uint32_t done = 0;
    while (done < budget && Dequeue(0)) ++done;
    return done;
}

IoStatus Win32IocpBlockSource::Poll(const BlockToken token, BlockView& view) noexcept {
    view = {};
    auto* lease = Find(token);
    if (lease == nullptr) return IoStatus::InvalidToken;
    auto& block = blocks_[lease->block];
    if (block.state == State::Pending) (void)Pump(1);
    if (block.state == State::Pending) return IoStatus::Pending;
    if (block.state == State::Failed) {
        error_ = block.error;
        return block.error == ERROR_OPERATION_ABORTED ? IoStatus::Cancelled : IoStatus::OsError;
    }
    view = {block.data, block.offset, block.bytes, token};
    return IoStatus::Ready;
}

bool Win32IocpBlockSource::DetachPollingThread() noexcept {
    const HANDLE temporary = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!temporary) { error_ = GetLastError(); return false; }
    DWORD bytes = 0; ULONG_PTR key = 0; OVERLAPPED* overlapped = nullptr;
    const BOOL result = GetQueuedCompletionStatus(temporary, &bytes, &key, &overlapped, 0);
    const DWORD status = result ? ERROR_SUCCESS : GetLastError();
    CloseHandle(temporary);
    if (status != WAIT_TIMEOUT) { error_ = status; return false; }
    return true;
}

bool Win32IocpBlockSource::Release(const BlockToken token) noexcept {
    auto* lease = Find(token);
    if (lease == nullptr) return false;
    auto& block = blocks_[lease->block];
    lease->active = false; --leased_;
    // The last holder of a failed read lets the block go; a read still in flight completes into the cache.
    if (--block.leases == 0 && block.state == State::Failed) block.state = State::Empty;
    return true;
}

void Win32IocpBlockSource::BeginCancel() noexcept {
    cancelling_ = true;
    if (file_ != INVALID_HANDLE_VALUE && pending_ != 0 && !CancelIoEx(file_, nullptr)) {
        const auto error = GetLastError();
        if (error != ERROR_NOT_FOUND) error_ = error;
    }
}

IoStatus Win32IocpBlockSource::Close() noexcept {
    if (pending_ != 0 || leased_ != 0) return IoStatus::WouldBlock;
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    if (port_ != nullptr) CloseHandle(port_);
    file_ = INVALID_HANDLE_VALUE;
    port_ = nullptr;
    blocks_ = nullptr; leases_ = nullptr;
    count_ = 0; lease_count_ = 0;
    size_ = 0;
    block_bytes_ = 0;
    return IoStatus::Ready;
}
} // namespace media::platform::windows
