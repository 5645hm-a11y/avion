#include "media/platform/OutputFile.hpp"
#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <cstddef>
#include <cstring>
#include <iterator>

namespace media::platform {
#if defined(_WIN32)
namespace {
bool Wide(const char* utf8, wchar_t* out, int capacity) noexcept {
    return utf8 && *utf8 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, capacity) > 0;
}
}
bool OutputFile::Create(const char* utf8_path, bool replace) noexcept {
    (void)Close();
    wchar_t wide[2048];
    if (!Wide(utf8_path, wide, 2048)) return false;
    const HANDLE file = CreateFileW(wide, GENERIC_WRITE, 0, nullptr, replace ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    handle_ = file; good_ = true;
    return true;
}
bool OutputFile::Write(const void* bytes, std::size_t count) noexcept {
    if (!handle_ || !good_) return false;
    const auto* p = static_cast<const std::byte*>(bytes);
    while (count) {
        const DWORD chunk = count > (1u << 30) ? (1u << 30) : static_cast<DWORD>(count);
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(handle_), p, chunk, &written, nullptr) || !written) { good_ = false; return false; }
        p += written; count -= written;
    }
    return true;
}
bool OutputFile::Close() noexcept {
    if (!handle_) return false;
    const bool closed = CloseHandle(static_cast<HANDLE>(handle_)) != FALSE;
    handle_ = nullptr;
    const bool good = good_ && closed; good_ = false;
    return good;
}
bool OutputFile::Remove(const char* utf8_path) noexcept {
    wchar_t wide[2048];
    return Wide(utf8_path, wide, 2048) && DeleteFileW(wide) != FALSE;
}
bool DefaultSnapshotFolder(char* out, std::size_t capacity) noexcept {
    if (!out || capacity < 2) return false;
    wchar_t folder[1024];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", folder, 1024);
    static constexpr wchar_t kPictures[] = L"\\Pictures", kOwn[] = L"\\MediaEngine";
    if (!n || n + std::size(kPictures) + std::size(kOwn) + 1 > 1024) return false;
    std::memcpy(folder + n, kPictures, sizeof(kPictures));
    if (!CreateDirectoryW(folder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    std::memcpy(folder + n + std::size(kPictures) - 1, kOwn, sizeof(kOwn));
    if (!CreateDirectoryW(folder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const auto length = n + std::size(kPictures) - 1 + std::size(kOwn) - 1;
    folder[length] = L'\\'; folder[length + 1] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, folder, -1, out, static_cast<int>(capacity), nullptr, nullptr) > 0;
}
#else
bool OutputFile::Create(const char* utf8_path, bool replace) noexcept {
    (void)Close();
    if (!utf8_path || !*utf8_path) return false;
    int fd = -1;
    do fd = ::open(utf8_path, O_WRONLY | O_CREAT | O_CLOEXEC | (replace ? O_TRUNC : O_EXCL), 0644); while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;
    fd_ = fd; good_ = true;
    return true;
}
bool OutputFile::Write(const void* bytes, std::size_t count) noexcept {
    if (fd_ < 0 || !good_) return false;
    const auto* p = static_cast<const std::byte*>(bytes);
    while (count) {
        const auto written = ::write(fd_, p, count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { good_ = false; return false; }
        p += written; count -= static_cast<std::size_t>(written);
    }
    return true;
}
bool OutputFile::Close() noexcept {
    if (fd_ < 0) return false;
    const bool closed = ::close(fd_) == 0;
    fd_ = -1;
    const bool good = good_ && closed; good_ = false;
    return good;
}
bool OutputFile::Remove(const char* utf8_path) noexcept { return utf8_path && ::unlink(utf8_path) == 0; }
bool DefaultSnapshotFolder(char* out, std::size_t capacity) noexcept {
    if (!out || !capacity) return false;
    out[0] = 0; // The working directory.
    return true;
}
#endif
}
