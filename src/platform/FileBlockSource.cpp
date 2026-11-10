#include "media/platform/FileBlockSource.hpp"
#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <algorithm>

namespace media::platform {
namespace {
bool SeekTo(std::FILE* f, std::uint64_t offset) noexcept {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
}
std::FILE* OpenUtf8(const char* utf8_path) noexcept {
    if (!utf8_path || !*utf8_path) return nullptr;
#if defined(_WIN32)
    wchar_t wide[2048];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, wide, 2048) <= 0) return nullptr;
    std::FILE* f = nullptr;
    return _wfopen_s(&f, wide, L"rb") == 0 ? f : nullptr;
#else
    return std::fopen(utf8_path, "rb");
#endif
}
bool FileBlockSource::Open(core::LinearMemoryArena& arena, const char* utf8_path, std::uint32_t block_bytes) noexcept {
    block_bytes_ = block_bytes == kLargeBlockBytes ? kLargeBlockBytes : kBlockBytes;
    Close();
    file_ = OpenUtf8(utf8_path);
    if (!file_) return false;
    std::setvbuf(file_, nullptr, _IONBF, 0); // No stdio buffer: nothing is allocated by a later read.
#if defined(_WIN32)
    if (_fseeki64(file_, 0, SEEK_END) != 0) { Close(); return false; }
    const long long end = _ftelli64(file_);
#else
    if (fseeko(file_, 0, SEEK_END) != 0) { Close(); return false; }
    const long long end = static_cast<long long>(ftello(file_));
#endif
    if (end <= 0) { Close(); return false; }
    size_ = static_cast<std::uint64_t>(end);
    for (auto& s : slots_) {
        s = {}; s.data = static_cast<std::byte*>(arena.Allocate(block_bytes_, 64));
        if (!s.data) { Close(); return false; }
    }
    return true;
}
void FileBlockSource::Close() noexcept {
    if (file_) std::fclose(file_);
    file_ = nullptr; size_ = 0;
    for (auto& s : slots_) s.active = false;
}
bool FileBlockSource::ReadAt(std::uint64_t offset, void* out, std::uint32_t bytes) noexcept {
    if (!file_ || offset > size_ || bytes > size_ - offset || !SeekTo(file_, offset)) return false;
    const bool good = std::fread(out, 1, bytes, file_) == bytes;
    read_ += good ? bytes : 0;
    return good;
}
IoStatus FileBlockSource::Request(std::uint64_t offset, std::uint32_t bytes, BlockToken& token) noexcept {
    token = {};
    if (!file_) return IoStatus::Closed;
    if (!bytes || offset > size_ || bytes > size_ - offset) return IoStatus::OutOfRange;
    if (bytes > block_bytes_) return IoStatus::TooLarge;
    for (std::uint32_t i = 0; i < kSlots; ++i) {
        auto& s = slots_[i];
        if (s.active) continue;
        if (!ReadAt(offset, s.data, bytes)) return IoStatus::OsError;
        s.active = true; ++s.generation;
        token = {i, s.generation};
        s.view = {s.data, offset, bytes, token};
        return IoStatus::Pending;
    }
    return IoStatus::WouldBlock;
}
IoStatus FileBlockSource::Poll(BlockToken token, BlockView& view) noexcept {
    if (token.id >= kSlots || !slots_[token.id].active || slots_[token.id].generation != token.generation) return IoStatus::InvalidToken;
    view = slots_[token.id].view;
    return IoStatus::Ready;
}
bool FileBlockSource::Release(BlockToken token) noexcept {
    if (token.id >= kSlots || !slots_[token.id].active || slots_[token.id].generation != token.generation) return false;
    slots_[token.id].active = false;
    return true;
}
bool ReadWholeFile(const char* utf8_path, void* out, std::size_t capacity, std::size_t& bytes) noexcept {
    bytes = 0;
    if (!utf8_path || !out) return false;
#if defined(_WIN32)
    wchar_t wide[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, wide, 1024) <= 0) return false;
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) <= capacity;
    std::size_t done = 0;
    while (ok && done < static_cast<std::size_t>(size.QuadPart)) {
        DWORD read = 0;
        const DWORD ask = static_cast<DWORD>((std::min)(static_cast<std::size_t>(size.QuadPart) - done, std::size_t{1} << 30));
        ok = ReadFile(file, static_cast<char*>(out) + done, ask, &read, nullptr) && read;
        done += read;
    }
    CloseHandle(file);
    bytes = done;
    return ok;
#else
    const int file = ::open(utf8_path, O_RDONLY);
    if (file < 0) return false;
    bool ok = true; std::size_t done = 0;
    for (;;) {
        if (done == capacity) { char extra; ok = ::read(file, &extra, 1) == 0; break; } // Larger than the buffer: refused.
        const auto got = ::read(file, static_cast<char*>(out) + done, capacity - done);
        if (got < 0) { ok = false; break; }
        if (got == 0) break;
        done += static_cast<std::size_t>(got);
    }
    ::close(file);
    bytes = done;
    return ok;
#endif
}
}
